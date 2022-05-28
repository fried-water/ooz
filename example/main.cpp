#include "ooze/core.h"

#include <chrono>
#include <thread>

namespace {

struct Point {
  int x;
  int y;
};

struct Box {
  Point min;
  Point max;
};

Point add(Point a, Point b) { return {a.x + b.x, a.y + b.y}; }
Point scale(Point a, int s) { return {a.x * s, a.y * s}; }

int sleep(int x) {
  std::this_thread::sleep_for(std::chrono::duration<int, std::ratio<1>>(x));
  return x;
}

ooze::Env create_env() {
  ooze::Env e = ooze::create_primative_env();

  e.add<Point>("Point");
  e.add<Box>("Box");

  e.add("add", add);
  e.add("scale", scale);
  e.add("sleep", sleep);

  return e;
}

} // namespace

int main(int argc, char* argv[]) {
  try {
    return ooze::main(argc, argv, create_env());
  } catch(const std::exception& e) {
    fmt::print("Error: uncaught exception {}\n", e.what());
  } catch(...) {
    fmt::print("Error: unknown exception\n");
  }

  return 1;
}
