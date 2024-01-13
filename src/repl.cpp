#include "pch.h"

#include "bindings.h"
#include "function_graph_construction.h"
#include "io.h"
#include "ooze/core.h"
#include "ooze/executor/task_executor.h"
#include "parser.h"
#include "parser_combinators.h"
#include "pretty_print.h"
#include "repl.h"
#include "sema.h"
#include "type_check.h"

#include <iostream>
#include <map>

namespace ooze {

namespace {

struct Command {
  bool run_main = false;
  std::vector<std::string> filenames;
};

std::optional<Command> parse_cmd_line(int argc, const char** argv) {
  if(argc <= 1) {
    return Command{};
  } else {
    const std::string_view cmd = argv[1];

    std::vector<std::string> filenames;
    for(int i = 2; i < argc; i++) {
      filenames.push_back(argv[i]);
    }

    if(cmd == "run") {
      return Command{true, std::move(filenames)};
    } else if(cmd == "repl") {
      return Command{false, std::move(filenames)};
    } else {
      return std::nullopt;
    }
  }
}

StringResult<void, Env> parse_scripts(Env e, const std::vector<std::string>& filenames) {
  std::vector<StringResult<std::string>> srcs = transform_to_vec(filenames, read_text_file);

  std::vector<std::string> errors = knot::accumulate(srcs, std::vector<std::string>{}, [](auto acc, const auto& r) {
    return r ? std::move(acc) : to_vec(std::move(r.error()), std::move(acc));
  });

  return errors.empty()
           ? parse_scripts(std::move(e),
                           transform_to_vec(srcs, [](const auto& r) { return std::string_view{r.value()}; }))
           : StringResult<void, Env>{Failure{std::move(errors)}, std::move(e)};
}

struct HelpCmd {};
struct EvalCmd {
  std::string file;
};
struct BindingsCmd {};
struct FunctionsCmd {};
struct TypesCmd {};
struct ReleaseCmd {
  std::string var;
};
struct AwaitCmd {
  std::vector<std::string> bindings;
};

auto help_parser() { return pc::construct<HelpCmd>(pc::constant("h", "h")); }

auto eval_parser() { return pc::construct<EvalCmd>(pc::seq(pc::constant("e", "e"), pc::any())); }

auto bindings_parser() { return pc::construct<BindingsCmd>(pc::constant("b", "b")); }

auto functions_parser() { return pc::construct<FunctionsCmd>(pc::constant("f", "f")); }

auto types_parser() { return pc::construct<TypesCmd>(pc::constant("t", "t")); }

auto release_parser() { return pc::construct<ReleaseCmd>(pc::seq(pc::constant("r", "r"), pc::any())); }

auto await_parser() { return pc::construct<AwaitCmd>(pc::seq(pc::constant("a", "a"), pc::n(pc::any()))); }

auto cmd_parser() {
  return pc::choose(
    help_parser(),
    eval_parser(),
    bindings_parser(),
    functions_parser(),
    types_parser(),
    release_parser(),
    await_parser());
}

auto parse_command(std::string_view line) {
  std::vector<std::string> split;

  auto pos = line.begin();
  while(pos != line.end()) {
    auto next_pos = std::find_if(pos, line.end(), [](char c) { return c == ' '; });
    split.emplace_back(pos, next_pos);
    pos = next_pos == line.end() ? line.end() : next_pos + 1;
  }

  return pc::parse(cmd_parser(), Span<std::string>{split}).map_error([](const auto&) {
    return std::vector<std::string>{"Error parsing command"};
  });
}

std::tuple<std::vector<std::string>, Env, Bindings> run(ExecutorRef, Env env, Bindings bindings, const HelpCmd& help) {
  return std::tuple(
    std::vector<std::string>{
      {":h - This message"},
      {":b - List all bindings (* means they are not ready, & means they are borrowed)"},
      {":f - List all environment and script functions"},
      {":t - List all registered types and their capabilities"},
      {":r binding - Release the given binding"},
      {":a bindings... - Await the given bindings or everything if unspecified"}},
    std::move(env),
    std::move(bindings));
}

std::tuple<std::vector<std::string>, Env, Bindings> run(ExecutorRef, Env env, Bindings bindings, BindingsCmd) {
  std::vector<std::string> ordered = sorted(transform_to_vec(bindings, [](const auto& p) { return p.first; }));

  std::vector<std::string> output;
  output.reserve(bindings.size() + 1);
  output.push_back(fmt::format("{} binding(s)", bindings.size()));

  for(const std::string& binding_name : ordered) {
    std::stringstream tree_ss;

    const auto& binding = bindings.at(binding_name);
    const BindingState max_state = std::accumulate(
      binding.values.begin(), binding.values.end(), BindingState::Ready, [](BindingState acc, const AsyncValue& ele) {
        const BindingState ele_state = find_binding_state(ele);
        return i32(acc) > i32(ele_state) ? acc : ele_state;
      });

    tree_ss << pretty_print(make_sv_array(env.src), env.tg, env.native_types.names, binding.type);
    output.push_back(fmt::format(
      "  {}: {}{}",
      binding_name,
      max_state == BindingState::Ready ? "" : (max_state == BindingState::Borrowed ? "&" : "*"),
      std::move(tree_ss).str()));
  }

  return std::tuple(std::move(output), std::move(env), std::move(bindings));
}

constexpr auto convert_errors = [](std::vector<std::string> errors, auto&&... ts) {
  return success(knot::Type<std::vector<std::string>>{}, std::move(errors), std::forward<decltype(ts)>(ts)...);
};

std::tuple<std::vector<std::string>, Env, Bindings> run(ExecutorRef, Env env, Bindings bindings, const EvalCmd& eval) {
  return read_text_file(eval.file)
    .append_state(std::move(env))
    .and_then([](std::string script, Env env) { return parse_scripts(std::move(env), make_sv_array(script)); })
    .map([](Env env) { return std::tuple(std::vector<std::string>{}, std::move(env)); })
    .or_else(convert_errors)
    .append_state(std::move(bindings))
    .value_and_state();
}

std::tuple<std::vector<std::string>, Env, Bindings> run(ExecutorRef, Env env, Bindings bindings, const FunctionsCmd&) {
  std::vector<std::pair<std::string, std::string>> functions;

  const std::array<std::string, 4> COLLAPSE{{"clone", "to_string", "serialize", "deserialize"}};

  for(const auto& [name, fs] : env.functions) {
    if(std::find(COLLAPSE.begin(), COLLAPSE.end(), name) == COLLAPSE.end()) {
      for(const auto& f : fs) {
        functions.emplace_back(
          name, fmt::format("{}{} -> {}", name, pretty_print(env, *f.type.input), pretty_print(env, *f.type.output)));
      }
    }
  }

  std::vector<std::string> output{fmt::format("{} function(s)", functions.size())};

  for(const std::string& name : COLLAPSE) {
    if(const auto it = env.functions.find(name); it != env.functions.end()) {
      output.push_back(fmt::format("  {} [{} overloads]", name, it->second.size()));
    }
  }

  for(const auto& [name, str] : sorted(std::move(functions))) {
    output.push_back(fmt::format("  {}", str));
  }

  return std::tuple(std::move(output), std::move(env), std::move(bindings));
}

std::tuple<std::vector<std::string>, Env, Bindings> run(ExecutorRef, Env env, Bindings bindings, const TypesCmd&) {
  std::map<std::string, bool> types;

  for(const auto& [id, name] : env.type_names) {
    TypedFunction to_string_wrap{
      TypedPattern{ast::Ident{"x"}, borrow_type(leaf_type(id))},
      {TypedCallExpr{{{ast::Ident{"to_string"}}}, {{std::vector{TypedExpr{ast::Ident{"x"}}}}}},
       leaf_type(type_id(knot::Type<std::string>{}))}};

    types[pretty_print(env, id)] =
      type_check(env, std::move(to_string_wrap))
        .and_then([&](TypedFunction f) { return overload_resolution(env, f); })
        .has_value();
  }

  std::vector<std::string> output{fmt::format("{} type(s)", types.size())};
  for(const auto& [name, info] : types) {
    output.push_back(fmt::format("  {:20} [to_string: {}]", name, info ? "Y" : "N"));
  }

  return std::tuple(std::move(output), std::move(env), std::move(bindings));
}

std::tuple<std::vector<std::string>, Env, Bindings>
run(ExecutorRef, Env env, Bindings bindings, const ReleaseCmd& cmd) {
  if(const auto it = bindings.find(cmd.var); it != bindings.end()) {
    bindings.erase(it);
    return std::tuple(std::vector<std::string>{}, std::move(env), std::move(bindings));
  } else {
    return std::tuple(make_vector(fmt::format("Binding {} not found", cmd.var)), std::move(env), std::move(bindings));
  }
}

std::tuple<std::vector<std::string>, Env, Bindings>
run(ExecutorRef executor, Env env, Bindings bindings, const AwaitCmd& cmd) {
  std::vector<std::string> output;
  if(cmd.bindings.empty()) {
    for(auto& [name, binding] : bindings) {
      for(AsyncValue& v : binding.values) {
        v = await(std::move(v));
      }
    }
  } else {
    for(const std::string& binding : cmd.bindings) {
      if(const auto it = bindings.find(binding); it != bindings.end()) {
        for(AsyncValue& v : it->second.values) {
          v = await(std::move(v));
        }
      } else {
        output.push_back(fmt::format("Binding {} not found", binding));
      }
    }
  }

  return std::tuple(std::move(output), std::move(env), std::move(bindings));
}

} // namespace

std::tuple<std::vector<std::string>, Env, Bindings>
step_repl(ExecutorRef executor, Env env, Bindings bindings, std::string_view line) {
  if(line.empty()) {
    return std::tuple(std::vector<std::string>{}, std::move(env), std::move(bindings));
  } else if(line[0] == ':') {
    return parse_command({line.data() + 1, line.size() - 1})
      .append_state(std::move(env), std::move(bindings))
      .map(visited([&](const auto& cmd, Env env, Bindings bindings) {
        return run(executor, std::move(env), std::move(bindings), cmd);
      }))
      .or_else(convert_errors)
      .value_and_state();
  } else {
    return run_to_string(executor, std::move(env), std::move(bindings), line)
      .map([](std::string out, Env env, Bindings bindings) {
        return std::tuple(
          out.empty() ? std::vector<std::string>{} : make_vector(std::move(out)), std::move(env), std::move(bindings));
      })
      .or_else(convert_errors)
      .value_and_state();
  }
}

std::tuple<Env, Bindings> run_repl(ExecutorRef executor, Env env, Bindings bindings) {
  fmt::print("Welcome to the ooze repl!\n");
  fmt::print("Try :h for help. Use Ctrl^D to exit.\n");
  fmt::print("> ");

  std::string line;
  std::vector<std::string> output;

  while(std::getline(std::cin, line)) {
    std::tie(output, env, bindings) = step_repl(executor, std::move(env), std::move(bindings), line);

    for(const auto& line : output) {
      fmt::print("{}\n", line);
    }

    fmt::print("> ");
  }

  return {std::move(env), std::move(bindings)};
}

int repl_main(int argc, const char** argv, Env e) {
  const std::optional<Command> cmd = parse_cmd_line(argc, argv);

  if(!cmd) {
    const char* msg =
      "Usage:\n"
      "  run [scripts...]\n"
      "  repl [scripts...]\n";

    fmt::print("{}", msg);
    return 1;
  }

  Executor executor = make_task_executor();

  const auto result =
    parse_scripts(std::move(e), cmd->filenames).append_state(Bindings{}).and_then([&](Env env, Bindings bindings) {
      if(cmd->run_main) {
        return run_to_string(executor, std::move(env), std::move(bindings), "main()")
          .map([](std::string s, Env e, Bindings b) {
            return std::tuple(make_vector(std::move(s)), std::move(e), std::move(b));
          });
      } else {
        std::tie(env, bindings) = run_repl(executor, std::move(env), std::move(bindings));
        return success(
          knot::Type<std::vector<std::string>>{}, std::vector<std::string>{}, std::move(env), std::move(bindings));
      }
    });

  for(const std::string& line : result ? result.value() : result.error()) {
    fmt::print("{}\n", line);
  }

  return !result.has_value();
}

} // namespace ooze
