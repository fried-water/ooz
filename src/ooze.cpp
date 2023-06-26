#include "pch.h"

#include "bindings.h"
#include "graph_construction.h"
#include "io.h"
#include "ooze/core.h"
#include "parser.h"
#include "parser_combinators.h"
#include "repl.h"
#include "type_check.h"
#include "user_msg.h"

#include <anyf/executor/task_executor.h>
#include <anyf/graph_execution.h>

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

Result<void> parse_scripts(Env& e, const std::vector<std::string>& filenames) {
  return knot::accumulate(
    filenames, Result<void>{}, [&](Result<void> acc, const std::string& filename) -> Result<void> {
      auto result =
        read_text_file(filename).and_then([&](const std::string& script) { return parse_script(e, script); });

      if(acc) {
        return result;
      } else if(result) {
        return acc;
      } else {
        return tl::unexpected{to_vec(std::move(result.error()), std::move(acc.error()))};
      }
    });
}

std::vector<std::string> gather_binding_strings(std::vector<Binding> bindings) {
  return transform_to_vec(std::move(bindings),
                          [](Binding b) { return anyf::any_cast<std::string>(take(std::move(b)).wait()); });
}

TypedFunction lift_only_borrowed_idents(TypedFunction f) {
  const auto count_usages = [&](const std::string& name) {
    return knot::preorder_accumulate(
      f.expr, 0, [&](int acc, const ast::IdentExpr& i) { return i.name == name ? acc + 1 : acc; });
  };

  const auto get_borrowed_usages = [&](const std::string& name) {
    return knot::preorder_accumulate(f.expr, std::vector<TypedExpr*>{}, [&](auto acc, TypedExpr& e) {
      knot::visit(e.v, [&](const TypedBorrowExpr& b) {
        knot::visit(b.expr->v, [&](const ast::IdentExpr& i) {
          if(i.name == name) {
            acc.push_back(&e);
          }
        });
      });
      return acc;
    });
  };

  co_visit(f.header.pattern,
           *f.header.type.input,
           [&](const ast::Pattern&, CompoundType<TypeID>& t, const ast::Ident& i, const auto&) {
             const int uses = count_usages(i.name);
             auto borrows = get_borrowed_usages(i.name);
             if(uses == borrows.size()) {
               t = borrow_type(std::move(t));
               for(TypedExpr* e : borrows) {
                 *e = TypedExpr{ast::IdentExpr{i.name}};
               }
             }
           });

  return f;
}

ContextualResult<TypedFunction>
infer_header_from_env(const RuntimeEnv& r, TypedExpr expr, CompoundType<TypeID> result_type) {
  TypedHeader h = inferred_header(expr);

  *h.type.output = std::move(result_type);

  std::vector<ContextualError> errors;

  co_visit(h.pattern,
           *h.type.input,
           Overloaded{[](ast::Pattern&, CompoundType<TypeID>&, const ast::WildCard& pattern, const auto& type) {
                        assert(false);
                      },
                      [&](ast::Pattern& p, CompoundType<TypeID>& t, const ast::Ident& ident, const auto&) {
                        if(const auto it = r.bindings.find(ident.name); it != r.bindings.end()) {
                          t = type(it->second);
                        } else {
                          errors.push_back({p.ref, fmt::format("use of undeclared binding '{}'", ident.name)});
                        }
                      }});

  return value_or_errors(TypedFunction{std::move(h), std::move(expr)}, std::move(errors));
}

ContextualResult<Tree<Binding>> run_function(RuntimeEnv& r, const CheckedFunction& f) {
  return create_graph(r.env, f).map([&](anyf::FunctionGraph g) {
    std::vector<anyf::Future> value_inputs;
    std::vector<anyf::BorrowedFuture> borrowed_inputs;

    co_visit(f.header.pattern,
             *f.header.type.input,
             Overloaded{[&](auto&, auto&, const ast::Ident& i, const auto&) {
                          value_inputs = to_vec(*take(r.bindings, i.name), std::move(value_inputs));
                        },
                        [&](auto&, auto&, const ast::Ident& i, const Borrow<TypeID>&) {
                          borrowed_inputs = to_vec(*borrow(r.bindings, i.name), std::move(borrowed_inputs));
                        }});

    std::vector<anyf::Future> results =
      anyf::execute_graph(g, r.executor, std::move(value_inputs), std::move(borrowed_inputs));

    int idx = 0;
    const auto converter =
      Overloaded{// result vector should be in order of a preorder traversal of the leaves in the output type
                 [&](TypeID t) -> Binding {
                   return {t, std::move(results[idx++])};
                 },

                 // These can't be part of the output type of executed functions
                 [](const FunctionType<TypeID>&) -> Binding {
                   assert(false);
                   exit(1);
                 },
                 [](const Floating&) -> Binding {
                   assert(false);
                   exit(1);
                 },
                 [](const Borrow<TypeID>&) -> Binding {
                   assert(false);
                   exit(1);
                 }};

    return knot::map<Tree<Binding>>(*f.header.type.output, std::cref(converter));
  });
}

ContextualResult<Tree<Binding>> run_expr(RuntimeEnv& r, TypedExpr expr, CompoundType<TypeID> type) {
  return infer_header_from_env(r, std::move(expr), std::move(type))
    .map(lift_only_borrowed_idents)
    .and_then([&](TypedFunction f) { return overload_resolution_concrete(r.env, std::move(f)); })
    .and_then([&](CheckedFunction f) { return run_function(r, f); });
}

ContextualResult<std::string> run_expr_to_string(RuntimeEnv& r, TypedExpr expr) {
  return infer_header_from_env(r, std::move(expr), floating_type<TypeID>())
    .map(lift_only_borrowed_idents)
    .and_then(
      [&](TypedFunction f) { return overload_resolution_concrete(r.env, f).map([&](auto) { return std::move(f); }); })
    .map([&](TypedFunction f) {
      TypedScopeExpr scope;

      if(std::holds_alternative<ast::IdentExpr>(f.expr.v)) {
        knot::visit(f.header.type.input->v, [](std::vector<CompoundType<TypeID>>& v) {
          assert(v.size() == 1);
          v[0] = borrow_type(std::move(v[0]));
        });

        scope.assignments.push_back(
          {{ast::Ident{"x"}}, borrow_type(std::move(*f.header.type.output)), std::move(f.expr)});
        scope.result = TypedExpr{TypedCallExpr{"to_string", {TypedExpr{ast::IdentExpr{"x"}}}}};
      } else {
        scope.assignments.push_back({{ast::Ident{"x"}}, std::move(*f.header.type.output), std::move(f.expr)});
        scope.result =
          TypedExpr{TypedCallExpr{"to_string", {TypedExpr{TypedBorrowExpr{TypedExpr{ast::IdentExpr{"x"}}}}}}};
      }

      f.header.type.output = leaf_type(anyf::type_id<std::string>());
      f.expr.v = std::move(scope);

      return f;
    })
    .and_then([&](TypedFunction f) { return overload_resolution_concrete(r.env, std::move(f)); })
    .and_then([&](CheckedFunction f) { return run_function(r, f); })
    .map([&](Tree<Binding> t) {
      std::vector<anyf::Future> futures = take(std::move(t));
      return anyf::any_cast<std::string>(std::move(futures[0]).wait());
    });
}

ContextualResult<void> run_assign(RuntimeEnv& r, TypedAssignment a) {
  return infer_header_from_env(r, std::move(*a.expr), std::move(a.type))
    .map(lift_only_borrowed_idents)
    .and_then([&](TypedFunction f) { return overload_resolution_concrete(r.env, std::move(f)); })
    .and_then([&](CheckedFunction f) {
      return type_check(r.env, a.pattern, *f.header.type.output).map([&](const auto&) { return std::move(f); });
    })
    .and_then([&](CheckedFunction f) { return run_function(r, f); })
    .map([&](Tree<Binding> results) {
      co_visit(a.pattern, results, [&](const ast::Pattern&, Tree<Binding>& tree, const ast::Ident& ident, const auto&) {
        r.bindings[ident.name] = std::move(tree);
      });
    });
}

struct TypeOfBindingConverter {
  CompoundType<TypeID> operator()(const Tree<Binding>& tree) {
    return {knot::map<decltype(CompoundType<TypeID>{}.v)>(tree.v, *this)};
  }

  TypeID operator()(const Binding& b) { return b.type; }
};

} // namespace

Tree<Any> await(Tree<Binding> tree) {
  return knot::map<Tree<Any>>(std::move(tree), [](Binding b) -> Any { return take(std::move(b)).wait(); });
}

CompoundType<TypeID> type(const Tree<Binding>& tree) { return TypeOfBindingConverter{}(tree); }

RuntimeEnv make_default_runtime(Env env) { return {std::move(env), anyf::make_task_executor()}; }

Result<void> parse_script(Env& e, std::string_view script) {
  return parse(script)
    .and_then([&](UnTypedAST ast) {
      std::vector<ContextualError> errors;

      std::vector<EnvFunctionRef> typed_ast;
      typed_ast.reserve(ast.size());

      for(const auto& [name, f] : ast) {
        if(auto typed_result = type_name_resolution(e, f); typed_result) {
          std::vector<EnvFunction>& functions = e.functions[name];
          functions.push_back({typed_result->header.type, std::move(*typed_result)});
          typed_ast.push_back({name, int(functions.size() - 1)});
        } else {
          errors = to_vec(std::move(typed_result.error()), std::move(errors));
        }
      }

      return value_or_errors(std::move(typed_ast), std::move(errors));
    })
    .and_then([&](std::vector<EnvFunctionRef> ast) {
      std::vector<ContextualError> errors;

      for(const auto& [name, idx, _] : ast) {
        EnvFunction& env_function = e.functions.at(name)[idx];

        auto graph_result =
          overload_resolution_concrete(e, std::move(std::move(std::get<TypedFunction>(env_function.f))))
            .and_then([&](const CheckedFunction& f) {
              env_function.type = f.header.type;
              return create_graph(e, f);
            })
            .map([&](anyf::FunctionGraph f) { env_function.f = std::move(f); });

        if(!graph_result) {
          errors = to_vec(std::move(graph_result.error()), std::move(errors));
        }
      }

      return errors.empty() ? ContextualResult<void>{} : tl::unexpected{std::move(errors)};
    })
    .map_error([&](auto errors) { return contextualize(script, std::move(errors)); });
}

Result<Tree<Binding>> run(RuntimeEnv& r, std::string_view expr) {
  return parse_expr(expr)
    .and_then([&](UnTypedExpr e) { return type_name_resolution(r.env, std::move(e)); })
    .and_then([&](TypedExpr e) { return run_expr(r, std::move(e), floating_type<TypeID>()); })
    .map_error([&](auto errors) { return contextualize(expr, std::move(errors)); });
}

Result<Tree<Binding>> run_or_assign(RuntimeEnv& r, std::string_view assignment_or_expr) {
  return parse_repl(assignment_or_expr)
    .and_then([&](auto var) {
      return std::visit(Overloaded{
                          [&](UnTypedExpr e) {
                            return type_name_resolution(r.env, std::move(e)).and_then([&](TypedExpr e) {
                              return run_expr(r, std::move(e), floating_type<TypeID>());
                            });
                          },
                          [&](UnTypedAssignment a) {
                            return type_name_resolution(r.env, std::move(a)).and_then([&](TypedAssignment a) {
                              return run_assign(r, std::move(a)).map([]() { return Tree<Binding>{}; });
                            });
                          },
                        },
                        std::move(var));
    })
    .map_error([&](auto errors) { return contextualize(assignment_or_expr, std::move(errors)); });
}

Result<std::string> run_to_string(RuntimeEnv& r, std::string_view expr) {
  return parse_expr(expr)
    .and_then([&](UnTypedExpr e) { return type_name_resolution(r.env, std::move(e)); })
    .and_then([&](TypedExpr e) { return run_expr_to_string(r, std::move(e)); })
    .map_error([&](auto errors) { return contextualize(expr, std::move(errors)); });
}

Result<std::string> run_to_string_or_assign(RuntimeEnv& r, std::string_view assignment_or_expr) {
  return parse_repl(assignment_or_expr)
    .and_then([&](auto var) {
      return std::visit(Overloaded{
                          [&](UnTypedExpr e) {
                            return type_name_resolution(r.env, std::move(e)).and_then([&](TypedExpr e) {
                              return run_expr_to_string(r, std::move(e));
                            });
                          },
                          [&](UnTypedAssignment a) {
                            return type_name_resolution(r.env, std::move(a)).and_then([&](TypedAssignment a) {
                              return run_assign(r, std::move(a)).map([]() { return std::string{}; });
                            });
                          },
                        },
                        std::move(var));
    })
    .map_error([&](auto errors) { return contextualize(assignment_or_expr, std::move(errors)); });
}

int main(int argc, const char** argv, Env e) {
  const std::optional<Command> cmd = parse_cmd_line(argc, argv);

  if(!cmd) {
    const char* msg = "Usage:\n"
                      "  run [scripts...]\n"
                      "  repl [scripts...]\n";

    fmt::print("{}", msg);
    return 1;
  }

  const auto result = parse_scripts(e, cmd->filenames).and_then([&]() {
    if(cmd->run_main) {
      RuntimeEnv r = make_default_runtime(std::move(e));
      return run_to_string(r, "main()").map([](std::string s) { return make_vector(std::move(s)); });
    } else {
      run_repl(make_default_runtime(std::move(e)));
      return Result<std::vector<std::string>>{};
    }
  });

  if(result) {
    dump(result.value());
  } else {
    dump(result.error());
  }

  return !result.has_value();
}

} // namespace ooze
