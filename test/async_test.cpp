#include "test.h"

#include "async_functions.h"
#include "graph_inner.h"
#include "ooze/executor/sequential_executor.h"
#include "ooze/executor/task_executor.h"
#include "ooze/executor/tbb_executor.h"

#include <algorithm>
#include <chrono>
#include <numeric>
#include <random>

namespace ooze {

namespace {

template <typename F>
AsyncFn to_async(F f) {
  return create_async(std::make_shared<AnyFunction>(std::move(f)));
}

template <typename... Ts, size_t... Is>
void compare(const std::tuple<Ts...>& exp, Span<Any> act, std::index_sequence<Is...>) {
  BOOST_REQUIRE_EQUAL(sizeof...(Ts), act.size());
  const auto compare = [&](const Any& any, const auto& t) {
    BOOST_CHECK(any_cast<std::decay_t<decltype(t)>>(any) == t);
  };
  (compare(act[Is], std::get<Is>(exp)), ...);
}

template <typename... Ts>
void compare(const std::tuple<Ts...>& exp, Span<Any> act) {
  compare(exp, act, std::make_index_sequence<sizeof...(Ts)>());
}

template <typename T>
void compare(const T& exp, Span<Any> act) {
  compare(std::tuple(exp), act);
}

template <typename... Ts, size_t... Is>
std::vector<Future> to_futures(ExecutorRef ex, std::tuple<Ts...> ts, std::index_sequence<Is...>) {
  std::vector<Future> futures;
  futures.reserve(sizeof...(Ts));
  const auto append = [&](auto t) { futures.push_back(Future(ex, Any(std::move(t)))); };
  (append(std::move(std::get<Is>(ts))), ...);
  return futures;
}

template <typename... Ts, typename... Bs>
std::vector<Any> run_async_fn(ExecutorRef ex, AsyncFn fn, std::tuple<Ts...> ts, std::tuple<Bs...> bs) {
  return transform_to_vec(
    fn(ex,
       to_futures(ex, std::move(ts), std::make_index_sequence<sizeof...(Ts)>()),
       transform_to_vec(to_futures(ex, std::move(bs), std::make_index_sequence<sizeof...(Bs)>()),
                        [](Future f) { return borrow(std::move(f)).first; })),
    [](Future f) { return std::move(f).wait(); });
}

template <typename... Ts, typename... Bs>
std::vector<Any> run_async_fn(AsyncFn fn, std::tuple<Ts...> ts, std::tuple<Bs...> bs) {
  return run_async_fn(make_seq_executor(), std::move(fn), std::move(ts), std::move(bs));
}

std::vector<int> create_vector(int size) {
  std::mt19937 rng;
  std::uniform_int_distribution<> dist(0, 100'000);
  std::vector<int> result;
  result.reserve(size);
  for(int i = 0; i < size; i++) {
    result.push_back(dist(rng));
  }

  return result;
}

auto create_shuffle(int seed) {
  return [seed](std::vector<int> vec) {
    std::shuffle(vec.begin(), vec.end(), std::mt19937(seed));
    return vec;
  };
}

i64 accumulate(const std::vector<int>& v) { return std::accumulate(v.begin(), v.end(), i64(0)); }

auto create_pipeline(int seed) {
  auto [cg, size] = make_graph({false});
  const auto create_output = cg.add(to_async(create_vector), size, {PassBy::Copy}, 1);
  const auto shuffle_output = cg.add(to_async(create_shuffle(seed)), create_output, {PassBy::Move}, 1);
  const auto sort_output =
    cg.add(to_async([](std::vector<int> v) { return sorted(std::move(v)); }), shuffle_output, {PassBy::Move}, 1);
  const auto acc_output = cg.add(to_async(accumulate), sort_output, {PassBy::Borrow}, 1);
  return std::move(cg).finalize(acc_output, {PassBy::Copy});
}

auto create_graph() {
  auto [cg, size] = make_graph({false});

  const std::array ps = {
    cg.add(create_pipeline(0), size)[0],
    cg.add(create_pipeline(1), size)[0],
    cg.add(create_pipeline(2), size)[0],
    cg.add(create_pipeline(3), size)[0],
    cg.add(create_pipeline(4), size)[0],
    cg.add(create_pipeline(5), size)[0],
    cg.add(create_pipeline(6), size)[0],
    cg.add(create_pipeline(7), size)[0]};

  const auto sumf = to_async([](i64 x, i64 y) { return x + y; });

  const auto pbs = std::array{PassBy::Copy, PassBy::Copy};

  const auto o1 = cg.add(sumf, {ps[0], ps[1]}, pbs, 1)[0];
  const auto o2 = cg.add(sumf, {ps[2], ps[3]}, pbs, 1)[0];
  const auto o3 = cg.add(sumf, {ps[4], ps[5]}, pbs, 1)[0];
  const auto o4 = cg.add(sumf, {ps[6], ps[7]}, pbs, 1)[0];

  const auto o5 = cg.add(sumf, {o1, o2}, pbs, 1)[0];
  const auto o6 = cg.add(sumf, {o3, o4}, pbs, 1)[0];

  return create_async_graph(std::move(cg).finalize(cg.add(sumf, {o5, o6}, pbs, 1), {PassBy::Copy}));
}

template <typename MakeExecutor>
void execute_with_threads(AsyncFn fn, MakeExecutor f) {
  const int size = 500'000;
  const int max_threads = 8;

  for(int num_threads = 1; num_threads <= max_threads; num_threads++) {
    Executor e = f(num_threads);
    const auto t0 = std::chrono::steady_clock::now();
    const auto results = run_async_fn(e, fn, std::tuple(size), std::tuple());
    const auto t1 = std::chrono::steady_clock::now();
    fmt::print("{} THREADS: result is {} after {}us\n",
               num_threads,
               any_cast<i64>(results[0]),
               std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
  }
}

} // namespace

BOOST_AUTO_TEST_SUITE(async_graph)

BOOST_AUTO_TEST_CASE(example_tbb, *boost::unit_test::disabled()) {
  fmt::print("\nExecuting graph with TBB\n\n");
  execute_with_threads(create_graph(), [](int n) { return make_tbb_executor(n); });
}

BOOST_AUTO_TEST_CASE(example_task, *boost::unit_test::disabled()) {
  fmt::print("\nExecuting graph with custom task system\n\n");
  execute_with_threads(create_graph(), [](int n) { return make_task_executor(n); });
}

BOOST_AUTO_TEST_CASE(example_seq, *boost::unit_test::disabled()) {
  fmt::print("\nExecuting graph Sequentially\n\n");
  execute_with_threads(create_graph(), [](int) { return make_seq_executor(); });
}

BOOST_AUTO_TEST_CASE(test_executor_ref_count) {
  Executor e = make_seq_executor();

  ExecutorRef er1 = e;
  ExecutorRef er2 = e;

  BOOST_CHECK_EQUAL(2, e.ref_count());

  ExecutorRef er_copy = er1;
  ExecutorRef er_move = std::move(er2);

  BOOST_CHECK_EQUAL(4, e.ref_count());

  er1 = e.ref();
  er2 = e.ref();

  BOOST_CHECK_EQUAL(4, e.ref_count());

  static_cast<void>(er_copy);
  static_cast<void>(er_move);

  // ~Executor() shouldnt crash (ref count should be 0)
}

BOOST_AUTO_TEST_CASE(empty) {
  auto [cg, s] = make_graph({false});
  const auto g = std::move(cg).finalize(s, {PassBy::Copy});
  compare(7, run_async_fn(create_async_graph(g), std::tuple(7), {}));
}

BOOST_AUTO_TEST_CASE(copy) {
  const auto take = to_async([](int i) { return i; });
  auto [cg, s] = make_graph({false});
  const auto g = std::move(cg).finalize(cg.add(take, s, {PassBy::Copy}, 1), {PassBy::Copy});
  compare(7, run_async_fn(create_async_graph(g), std::tuple(7), {}));
}

BOOST_AUTO_TEST_CASE(move) {
  const auto take = to_async([](int i) { return i; });
  auto [cg, s] = make_graph({false});
  const auto g = std::move(cg).finalize(cg.add(take, s, {PassBy::Move}, 1), {PassBy::Move});
  compare(7, run_async_fn(create_async_graph(g), std::tuple(7), {}));
}

BOOST_AUTO_TEST_CASE(borrow) {
  const auto take_ref = to_async([](const int& i) { return i; });
  auto [cg, s] = make_graph({false});
  const auto g = std::move(cg).finalize(cg.add(take_ref, s, {PassBy::Borrow}, 1), {PassBy::Copy});
  compare(7, run_async_fn(create_async_graph(g), std::tuple(7), {}));
}

BOOST_AUTO_TEST_CASE(sentinal) {
  const auto take = to_async([](Sentinal sent) { return sent; });

  const auto borrow = to_async([](const Sentinal& sent) {
    BOOST_CHECK_EQUAL(0, sent.copies);
    return sent;
  });

  auto [cg, inputs] = make_graph({false, false, false});

  const Oterm o1 = cg.add(take, cg.add(take, {inputs[0]}, {PassBy::Move}, 1), {PassBy::Move}, 1)[0];
  const Oterm o2 = cg.add(take, {inputs[1]}, {PassBy::Copy}, 1)[0];
  const Oterm o3 = inputs[1];
  const Oterm o4 = cg.add(borrow, {inputs[2]}, {PassBy::Borrow}, 1)[0];
  const Oterm o5 = inputs[2];

  const auto g = create_async_graph(std::move(cg).finalize(
    {o1, o2, o3, o4, o5}, {PassBy::Move, PassBy::Move, PassBy::Move, PassBy::Move, PassBy::Move}));

  std::vector<Any> results = run_async_fn(g, std::tuple(Sentinal{}, Sentinal{}, Sentinal{}), {});

  BOOST_CHECK_EQUAL(0, any_cast<Sentinal>(results[0]).copies); // Move since inputs[0] not used elsewhere
  BOOST_CHECK_EQUAL(1, any_cast<Sentinal>(results[1]).copies); // Inputs[1] copied into take, moved out
  BOOST_CHECK_EQUAL(0, any_cast<Sentinal>(results[2]).copies); // Inputs[1] moved to output
  BOOST_CHECK_EQUAL(1, any_cast<Sentinal>(results[3]).copies); // inputs[2] must be copied through borrow
  BOOST_CHECK_EQUAL(0, any_cast<Sentinal>(results[4]).copies); // inputs[2] is moved after borrowed elsewhere
}

BOOST_AUTO_TEST_CASE(move_only) {
  const auto take = to_async([](std::unique_ptr<int> ptr) { return *ptr; });
  auto [cg, ptr] = make_graph({false});
  const auto g = create_async_graph(std::move(cg).finalize(cg.add(take, ptr, {PassBy::Move}, 1), {PassBy::Move}));
  compare(5, run_async_fn(g, std::tuple(std::make_unique<int>(5)), {}));
}

BOOST_AUTO_TEST_CASE(fwd) {
  const auto fwd = to_async([](Sentinal&& s) -> Sentinal&& { return std::move(s); });

  auto [cg, inputs] = make_graph({false});
  const auto g = create_async_graph(std::move(cg).finalize(cg.add(fwd, inputs, {PassBy::Move}, 1), {PassBy::Move}));

  Executor ex = make_seq_executor();
  std::vector<Future> results = g(ex, make_vector(Future(ex, Any(Sentinal{}))), {});
  BOOST_REQUIRE_EQUAL(1, results.size());
  const Sentinal result = any_cast<Sentinal>(std::move(results.front()).wait());

  BOOST_CHECK_EQUAL(0, result.copies);
  BOOST_CHECK_EQUAL(3, result.moves); // into input any, through fwd, into result
}

BOOST_AUTO_TEST_CASE(borrow_fwd) {
  Executor ex = make_seq_executor();

  auto [cg, inputs] = make_graph({true, true});
  const auto outputs = cg.add(
    to_async([](const Sentinal&, Sentinal) {}), std::array{inputs[0], inputs[0]}, {PassBy::Borrow, PassBy::Copy}, 0);

  const auto g = create_async_graph(std::move(cg).finalize({}, {}));

  auto [b1, post_future1] = ooze::borrow(Future(ex, Sentinal{}));
  auto [b2, post_future2] = ooze::borrow(Future(ex, Sentinal{}));
  const auto results = g(ex, {}, {std::move(b1), std::move(b2)});
  const Sentinal input1 = any_cast<Sentinal>(std::move(post_future1).wait());
  const Sentinal input2 = any_cast<Sentinal>(std::move(post_future2).wait());

  BOOST_CHECK_EQUAL(0, results.size());
  BOOST_CHECK_EQUAL(0, input1.copies);
  BOOST_CHECK_EQUAL(2, input1.moves);
  BOOST_CHECK_EQUAL(0, input2.copies);
  BOOST_CHECK_EQUAL(2, input2.moves);
}

BOOST_AUTO_TEST_CASE(timing, *boost::unit_test::disabled()) {
  const size_t COUNT = 5;

  auto [cg, input_terms] = make_graph(std::vector<bool>(COUNT, true));

  std::vector<Oterm> outputs;

  for(size_t i = 0; i < COUNT; i++) {
    outputs.push_back(cg.add(to_async([=](const std::string& s) {
                               std::this_thread::sleep_for(std::chrono::duration<int, std::milli>(i));
                               return s + " out";
                             }),
                             {input_terms[i]},
                             {PassBy::Borrow},
                             1)[0]);
  }

  const auto g = create_async_graph(std::move(cg).finalize(outputs, std::vector<PassBy>(COUNT, PassBy::Move)));

  Executor ex = make_task_executor();

  std::vector<Promise> promises;
  std::vector<BorrowedFuture> inputs;
  std::vector<Future> input_futures;

  for(size_t i = 0; i < COUNT; i++) {
    auto [p, f] = make_promise_future(ex);
    auto [b, bf] = ooze::borrow(std::move(f));
    promises.push_back(std::move(p));
    inputs.push_back(std::move(b));
    input_futures.push_back(std::move(bf));
  }

  auto futures = g(ex, {}, std::move(inputs));

  futures.insert(
    futures.end(), std::make_move_iterator(input_futures.begin()), std::make_move_iterator(input_futures.end()));

  std::mutex m;
  std::vector<std::pair<std::string, std::chrono::time_point<std::chrono::steady_clock>>> ordered_results;
  ordered_results.reserve(futures.size());

  std::vector<std::thread> threads;
  for(Future& f : futures) {
    threads.emplace_back([&]() {
      std::string str = any_cast<std::string>(std::move(f).wait());
      const auto time = std::chrono::steady_clock::now();
      std::lock_guard lk(m);
      ordered_results.emplace_back(std::move(str), time);
    });
  }

  auto start = std::chrono::steady_clock::now();

  for(size_t i = 0; i < COUNT; i++) {
    std::move(promises[i]).send(std::string(1, char('A' + i)));
  }

  for(std::thread& t : threads) {
    t.join();
  }

  for(const auto& [string, time] : ordered_results) {
    fmt::print("({:05} us) {}\n", std::chrono::duration_cast<std::chrono::microseconds>(time - start).count(), string);
  }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(async_fns)

BOOST_AUTO_TEST_CASE(value) {
  compare(1, run_async_fn(create_async_value(1), {}, {}));
  compare(std::string("abc"), run_async_fn(create_async_value(std::string("abc")), {}, {}));
}

BOOST_AUTO_TEST_CASE(any_function) {
  compare(std::tuple(), run_async_fn(to_async([]() {}), {}, {}));
  compare(3, run_async_fn(to_async([]() { return 3; }), {}, {}));
  compare(7, run_async_fn(to_async([](int x) { return x; }), std::tuple(7), {}));
  compare(7, run_async_fn(to_async([](const int& x) { return x; }), {}, std::tuple(7)));
  compare(9, run_async_fn(to_async([](int x, const int& y) { return x + y; }), std::tuple(2), std::tuple(7)));
}

BOOST_AUTO_TEST_CASE(functional) {
  compare(std::tuple(), run_async_fn(create_async_functional(0), std::tuple(to_async([]() {})), {}));
  compare(3, run_async_fn(create_async_functional(1), std::tuple(to_async([]() { return 3; })), {}));
  compare(7, run_async_fn(create_async_functional(1), std::tuple(to_async([](int x) { return x + 1; }), 6), {}));
  compare(
    7,
    run_async_fn(create_async_functional(1), std::tuple(to_async([](const int& x) { return x + 1; })), std::tuple(6)));
  compare(9,
          run_async_fn(create_async_functional(1),
                       std::tuple(to_async([](int x, const int& y) { return x + y + 1; }), 2),
                       std::tuple(6)));
}

BOOST_AUTO_TEST_CASE(select) {
  compare(std::tuple(), run_async_fn(create_async_select(), std::tuple(true), {}));
  compare(std::tuple(), run_async_fn(create_async_select(), std::tuple(false), {}));

  compare(1, run_async_fn(create_async_select(), std::tuple(true, 1, 2), {}));
  compare(2, run_async_fn(create_async_select(), std::tuple(false, 1, 2), {}));

  compare(std::tuple(1, 2), run_async_fn(create_async_select(), std::tuple(true, 1, 2, 3, 4), {}));
  compare(std::tuple(3, 4), run_async_fn(create_async_select(), std::tuple(false, 1, 2, 3, 4), {}));
}

BOOST_AUTO_TEST_CASE(if_) {
  compare(1, run_async_fn(create_async_if(1, create_async_value(1), create_async_value(2)), std::tuple(true), {}));
  compare(2, run_async_fn(create_async_if(1, create_async_value(1), create_async_value(2)), std::tuple(false), {}));

  const auto identity = to_async([](int x) { return x; });
  const auto add1 = to_async([](int x) { return x + 1; });

  compare(5, run_async_fn(create_async_if(1, identity, add1), std::tuple(true, 5), {}));
  compare(6, run_async_fn(create_async_if(1, identity, add1), std::tuple(false, 5), {}));

  const auto identity_borrow = to_async([](const int& x) { return x; });
  const auto add1_borrow = to_async([](const int& x) { return x + 1; });

  compare(5, run_async_fn(create_async_if(1, identity_borrow, add1_borrow), std::tuple(true), std::tuple(5)));
  compare(6, run_async_fn(create_async_if(1, identity_borrow, add1_borrow), std::tuple(false), std::tuple(5)));

  const auto add = to_async([](int x, const int& y) { return x + y; });
  const auto mul = to_async([](int x, const int& y) { return x * y; });

  compare(7, run_async_fn(create_async_if(1, add, mul), std::tuple(true, 3), std::tuple(4)));
  compare(12, run_async_fn(create_async_if(1, add, mul), std::tuple(false, 3), std::tuple(4)));
}

BOOST_AUTO_TEST_CASE(converge) {
  const auto empty_body = create_async_value(true);
  const auto body = to_async([](int x, const int& limit) { return std::tuple(x + 1 >= limit, x + 1); });

  compare(std::tuple(), run_async_fn(create_async_converge(), std::tuple(empty_body, false), {}));
  compare(std::tuple(), run_async_fn(create_async_converge(), std::tuple(empty_body, true), {}));
  compare(10, run_async_fn(create_async_converge(), std::tuple(body, false, 5), std::tuple(10)));
  compare(5, run_async_fn(create_async_converge(), std::tuple(body, true, 5), std::tuple(10)));
}

BOOST_AUTO_TEST_SUITE(stress)

constexpr int NUM_EXECUTIONS = 100;

BOOST_AUTO_TEST_CASE(any_function) {
  const auto fn = to_async([](int x, const int& y) { return x + y; });
  for(int i = 0; i < NUM_EXECUTIONS; i++) {
    compare(i + 5, run_async_fn(make_task_executor(), fn, std::tuple(5), std::tuple(i)));
  }
}

BOOST_AUTO_TEST_CASE(functional) {
  for(int i = 0; i < NUM_EXECUTIONS; i++) {
    compare(i + 12,
            run_async_fn(make_task_executor(),
                         create_async_functional(1),
                         std::tuple(to_async([i](int x, const int& y) { return x + y + i; }), 5),
                         std::tuple(7)));
  }
}

BOOST_AUTO_TEST_CASE(select) {
  for(int i = 0; i < NUM_EXECUTIONS; i++) {
    compare(i % 2, run_async_fn(make_task_executor(), create_async_select(), std::tuple(i % 2 == 0, 0, 1), {}));
  }
}

BOOST_AUTO_TEST_CASE(converge) {
  const auto body = to_async([](int x, const int& limit) { return std::tuple(x + 1 >= limit, x + 1); });

  for(int i = 0; i < NUM_EXECUTIONS; i++) {
    const int limit = i % 10;
    compare(
      limit,
      run_async_fn(make_task_executor(), create_async_converge(), std::tuple(body, 0 >= limit, 0), std::tuple(limit)));
  }
}

BOOST_AUTO_TEST_CASE(if_) {
  const auto identity = to_async([](int x) { return x; });
  const auto add1 = to_async([](int x) { return x + 1; });
  for(int i = 0; i < NUM_EXECUTIONS; i++) {
    compare(i % 2,
            run_async_fn(make_task_executor(), create_async_if(1, identity, add1), std::tuple(i % 2 == 0, 0), {}));
  }
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()

} // namespace ooze
