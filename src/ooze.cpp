#include "pch.h"

#include "async_functions.h"
#include "bindings.h"
#include "function_graph_construction.h"
#include "ooze/core.h"
#include "parser.h"
#include "parser_combinators.h"
#include "pretty_print.h"
#include "sema.h"
#include "type_check.h"
#include "user_msg.h"

namespace ooze {

namespace {

bool is_binding_copyable(const TypeGraph& tg, const std::unordered_set<TypeID>& copy_types, TypeRef type) {
  bool is_copyable = true;

  preorder(tg, type, [&](TypeRef t) {
    switch(tg.get<TypeTag>(t)) {
    case TypeTag::Leaf:
      is_copyable = is_copyable && copy_types.find(tg.get<TypeID>(t)) != copy_types.end();
      return false;
    case TypeTag::Fn: return false;
    case TypeTag::Floating: assert(false); return false;
    case TypeTag::Borrow: assert(false); return false;
    case TypeTag::Tuple: return true;
    }
    assert(false);
    return true;
  });

  return is_copyable;
}

TypeRef copy_type(Span<std::string_view> srcs, Env& env, Map<TypeRef, TypeRef>& m, const TypeGraph& tg, TypeRef type) {
  if(const auto it = m.find(type); it != m.end()) {
    return it->second;
  } else if(tg.get<TypeTag>(type) == TypeTag::Leaf) {
    const TypeID type_id = tg.get<TypeID>(type);
    const auto it = env.type_cache.native.find(type_id);
    return it != env.type_cache.native.end() ? it->second.first : env.tg.add_node(TypeTag::Leaf, type_id);
  } else {
    std::vector<TypeRef> children =
      transform_to_vec(tg.fanout(type), [&](TypeRef fanout) { return copy_type(srcs, env, m, tg, fanout); });

    const TypeRef new_type = env.tg.add_node(children, tg.get<TypeTag>(type), tg.get<TypeID>(type));
    m.emplace(type, new_type);
    return new_type;
  }
}

template <typename Parser>
ContextualResult<CallGraphData, AST, TypeGraph>
type_check(Parser p,
           Span<std::string_view> srcs,
           const TypeCache& tc,
           const NativeTypeInfo& native_types,
           AST ast,
           TypeGraph tg) {
  assert(srcs.size() == 2);

  return p(std::move(ast), std::move(tg), SrcID{1}, srcs[1])
    .and_then([&](auto type_srcs, AST ast, TypeGraph tg) {
      return type_name_resolution(srcs, native_types.names, type_srcs, std::move(tg)).map_state([&](TypeGraph tg) {
        return std::tuple(std::move(ast), std::move(tg));
      });
    })
    .and_then([&](AST ast, TypeGraph tg) { return sema(srcs, tc, native_types, std::move(ast), std::move(tg)); });
}

std::tuple<std::vector<AsyncValue>, Map<ASTID, std::vector<AsyncValue>>> run_function(
  Span<std::string_view> srcs,
  const AST& ast,
  const TypeGraph& tg,
  const Map<ASTID, ASTID>& binding_of,
  const std::unordered_set<TypeID>& copy_types,
  const std::unordered_map<ASTID, AsyncFn>& flat_functions,
  ExecutorRef ex,
  Map<ASTID, std::vector<AsyncValue>> bindings,
  ASTID expr_id) {
  assert(is_expr(ast.forest[expr_id]));

  auto [value_inputs, borrow_inputs, fg] = create_graph(ast, tg, copy_types, binding_of, expr_id);

  std::vector<BorrowedFuture> borrowed;
  for(ASTID id : borrow_inputs) {
    const auto it = bindings.find(id);
    assert(it != bindings.end());
    borrowed = transform_to_vec(
      it->second, [](AsyncValue& v) { return borrow(v); }, std::move(borrowed));
  }

  std::vector<Future> futures;
  for(ASTID id : value_inputs) {
    if(const auto it = bindings.find(id); it == bindings.end()) {
      const auto fn_it = flat_functions.find(id);
      assert(fn_it != flat_functions.end());
      futures.push_back(Future(Any(fn_it->second)));
    } else if(is_binding_copyable(tg, copy_types, ast.types[id.get()])) {
      for(AsyncValue& b : it->second) {
        futures = transform_to_vec(
          it->second, [](AsyncValue& v) { return borrow(v).then([](const Any& a) { return a; }); }, std::move(futures));
      }
    } else {
      futures = transform_to_vec(
        std::move(it->second), [](AsyncValue v) { return take(std::move(v)); }, std::move(futures));
      bindings.erase(it);
    }
  }

  AsyncFn fn = create_async_graph(std::move(fg));

  return std::tuple(transform_to_vec(fn(ex, std::move(futures), std::move(borrowed)), Construct<AsyncValue>{}),
                    std::move(bindings));
}

auto assign_values(const AST& ast,
                   const TypeGraph& tg,
                   Map<ASTID, std::vector<AsyncValue>> bindings,
                   std::vector<AsyncValue> values,
                   ASTID pattern) {
  int offset = 0;
  for(const ASTID id : ast.forest.leaf_ids(pattern)) {
    const int size = size_of(tg, ast.types[id.get()]);
    if(ast.forest[id] == ASTTag::PatternIdent) {
      bindings.emplace(id,
                       std::vector<AsyncValue>(std::make_move_iterator(values.begin() + offset),
                                               std::make_move_iterator(values.begin() + offset + size)));
    }
    offset += size;
  }
  assert(offset == values.size());
  return bindings;
}

auto run_or_assign(ExecutorRef ex,
                   Span<std::string_view> srcs,
                   const AST& ast,
                   const TypeGraph& tg,
                   const Map<ASTID, ASTID>& binding_of,
                   Map<TypeRef, TypeRef>& to_env_type,
                   Env env,
                   Map<ASTID, std::vector<AsyncValue>> bindings,
                   ASTID id) {
  assert(ast.forest.is_root(id));

  const bool expr = is_expr(ast.forest[id]);
  const TypeRef type = copy_type(srcs, env, to_env_type, tg, ast.types[id.get()]);

  std::vector<AsyncValue> values;
  std::tie(values, bindings) = run_function(
    srcs,
    ast,
    tg,
    binding_of,
    env.native_types.copyable,
    env.flat_functions,
    ex,
    std::move(bindings),
    expr ? id : ast.forest.child_ids(id).get<1>());

  return expr ? std::tuple(Binding{type, std::move(values)}, std::move(env), std::move(bindings))
              : std::tuple(Binding{type},
                           std::move(env),
                           assign_values(ast, tg, std::move(bindings), std::move(values), *ast.forest.first_child(id)));
}

Env generate_functions(
  Span<std::string_view> srcs, Env env, const AST& ast, const TypeGraph& tg, const CallGraphData& cg) {
  Map<TypeRef, TypeRef> to_env_type;

  // type graph was copied from env initially, so all those nodes should be identical
  for(TypeRef t : id_range(TypeRef{env.tg.num_nodes()})) {
    to_env_type.emplace(t, t);
  }

  Map<ASTID, ASTID> to_env_id;

  for(ASTID id : bfs_traversal(invert(cg.call_graph), cg.leaf_fns)) {
    const auto fn_id = ast.forest.next_sibling(id);
    assert(fn_id);

    if(ast.forest[*fn_id] == ASTTag::Fn) {
      const auto fn_id = ast.forest.next_sibling(id);
      assert(fn_id && ast.forest[*fn_id] == ASTTag::Fn);
      auto [global_values, global_borrows, fg] =
        create_graph(ast, tg, env.native_types.copyable, cg.binding_of, *fn_id);

      assert(global_borrows.empty());

      std::vector<Any> values = transform_to_vec(global_values, [&](ASTID id) {
        const auto it = to_env_id.find(id);
        assert(it != to_env_id.end());
        const auto fn_it = env.flat_functions.find(it->second);
        assert(fn_it != env.flat_functions.end());
        return Any(fn_it->second);
      });

      const TypeRef env_fn_type = copy_type(srcs, env, to_env_type, tg, ast.types[id.get()]);

      to_env_id.emplace(
        id,
        env.add_function(
          sv(srcs, ast.srcs[id.get()]), env_fn_type, curry(create_async_graph(std::move(fg)), std::move(values))));
    } else {
      to_env_id.emplace(id, id);
    }
  }

  return env;
}

std::tuple<std::string, AST, Map<ASTID, std::vector<AsyncValue>>>
append_global_bindings(std::string env_src, AST ast, Bindings str_bindings) {
  Map<ASTID, std::vector<AsyncValue>> bindings;
  for(auto& [name, binding] : str_bindings) {
    bindings.emplace(add_global(ast, SrcRef{SrcID{0}, append_src(env_src, name)}, binding.type),
                     std::move(binding.values));
  }

  return std::tuple(std::move(env_src), std::move(ast), std::move(bindings));
}

std::tuple<Env, Bindings> to_str_bindings(
  Span<std::string_view> srcs,
  const AST& ast,
  const TypeGraph& tg,
  Map<TypeRef, TypeRef>& to_env_type,
  Env env,
  Map<ASTID, std::vector<AsyncValue>> bindings) {
  Bindings str_bindings;
  for(auto& [id, values] : bindings) {
    str_bindings.emplace(std::string(sv(srcs, ast.srcs[id.get()])),
                         Binding{copy_type(srcs, env, to_env_type, tg, ast.types[id.get()]), std::move(values)});
  }
  return std::tuple(std::move(env), std::move(str_bindings));
}

} // namespace

StringResult<void> type_check_expr(const Env& env, std::string_view expr) {
  const auto srcs = make_sv_array(env.src, expr);
  return type_check(parse_expr, srcs, env.type_cache, env.native_types, env.ast, env.tg)
    .map_state(nullify())
    .map(nullify())
    .map_error([&](std::vector<ContextualError> errors) { return contextualize(srcs, std::move(errors)); });
}

StringResult<void> type_check_fn(const Env& env, std::string_view fn) {
  const auto srcs = make_sv_array(env.src, fn);
  return type_check(parse_function, srcs, env.type_cache, env.native_types, env.ast, env.tg)
    .map_state(nullify())
    .map(nullify())
    .map_error([&](std::vector<ContextualError> errors) { return contextualize(srcs, std::move(errors)); });
}

StringResult<void, Env> parse_scripts(Env env, Span<std::string_view> files) {
  const std::string env_src = env.src;
  const auto srcs = flatten(make_sv_array(env_src), files);

  return accumulate_errors<std::pair<TypeRef, SrcRef>, ContextualError>(
           [&srcs](SrcID src, AST ast, TypeGraph tg) {
             return parse(std::move(ast), std::move(tg), src, srcs[src.get()]);
           },
           id_range(SrcID(1), SrcID(srcs.size())),
           env.ast,
           env.tg)
    .and_then([&](auto type_srcs, AST ast, TypeGraph tg) {
      return type_name_resolution(srcs, env.native_types.names, type_srcs, std::move(tg)).map_state([&](TypeGraph tg) {
        return std::tuple(std::move(ast), std::move(tg));
      });
    })
    .and_then([&](AST ast, TypeGraph tg) {
      return sema(srcs, env.type_cache, env.native_types, std::move(ast), std::move(tg));
    })
    .append_state(std::move(env))
    .map([&](CallGraphData cg, AST ast, TypeGraph tg, Env env) {
      env = generate_functions(srcs, std::move(env), ast, tg, cg);
      return std::tuple(std::move(ast), std::move(tg), std::move(env));
    })
    .map_state([](AST, TypeGraph, Env env) { return env; })
    .map_error([&srcs](auto errors, Env env) {
      return std::tuple(contextualize(srcs, std::move(errors)), std::move(env));
    });
}

StringResult<Binding, Env, Bindings> run(ExecutorRef ex, Env env, Bindings str_bindings, std::string_view expr) {
  Map<TypeRef, TypeRef> to_env_type;

  auto [env_src, ast, bindings] = append_global_bindings(env.src, env.ast, std::move(str_bindings));

  const auto srcs = make_sv_array(env_src, expr);

  return type_check(parse_repl, srcs, env.type_cache, env.native_types, std::move(ast), env.tg)
    .append_state(std::move(env), std::move(bindings))
    .map([&](CallGraphData cg, AST ast, TypeGraph tg, Env env, Map<ASTID, std::vector<AsyncValue>> bindings) {
      Binding result;
      std::tie(result, env, bindings) = run_or_assign(
        ex,
        srcs,
        ast,
        tg,
        cg.binding_of,
        to_env_type,
        std::move(env),
        std::move(bindings),
        ASTID{i32(ast.forest.size() - 1)});
      return std::tuple(std::move(result), std::move(ast), std::move(tg), std::move(env), std::move(bindings));
    })
    .map_state([&](AST ast, TypeGraph tg, Env env, Map<ASTID, std::vector<AsyncValue>> bindings) {
      return to_str_bindings(srcs, ast, tg, to_env_type, std::move(env), std::move(bindings));
    })
    .map_error([&](auto errors, Env env, Bindings bindings) {
      return std::tuple(contextualize(srcs, std::move(errors)), std::move(env), std::move(bindings));
    });
}

StringResult<std::string, Env, Bindings>
run_to_string(ExecutorRef ex, Env env, Bindings str_bindings, std::string_view expr) {
  Map<TypeRef, TypeRef> to_env_type;

  auto [env_src, ast, bindings] = append_global_bindings(env.src, env.ast, std::move(str_bindings));
  const SrcRef to_string_ref = {SrcID{0}, append_src(env_src, "to_string")};
  const SrcRef string_ref = {SrcID{0}, append_src(env_src, "string")};

  const auto srcs = make_sv_array(env_src, expr);

  return type_check(parse_repl, srcs, env.type_cache, env.native_types, std::move(ast), env.tg)
    .and_then([&](CallGraphData cg, AST ast, TypeGraph tg) {
      const auto id = ASTID{i32(ast.forest.size() - 1)};
      if(ast.forest[id] == ASTTag::Assignment) {
        return success(knot::Type<std::vector<ContextualError>>{}, std::move(cg), std::move(ast), std::move(tg));
      } else {
        const TypeRef expr_type = ast.types[id.get()];
        const TypeRef borrow_type = tg.add_node(std::array{expr_type}, TypeTag::Borrow, TypeID{});
        const TypeRef tuple_type = tg.add_node(std::array{borrow_type}, TypeTag::Tuple, TypeID{});
        const TypeRef string_type = tg.add_node(TypeTag::Leaf, type_id(knot::Type<std::string>{}));
        const TypeRef fn_type = tg.add_node(std::array{tuple_type, string_type}, TypeTag::Fn, TypeID{});

        const ASTID borrow_id = append_root(ast, ASTTag::ExprBorrow, SrcRef{}, borrow_type, std::array{id});
        const ASTID tuple_id = append_root(ast, ASTTag::ExprTuple, SrcRef{}, tuple_type, std::array{borrow_id});
        const ASTID callee_id = append_root(ast, ASTTag::ExprIdent, to_string_ref, fn_type);
        const ASTID fn_id = append_root(ast, ASTTag::ExprCall, SrcRef{}, string_type, std::array{callee_id, tuple_id});

        return sema(srcs, env.type_cache, env.native_types, std::move(ast), std::move(tg));
      }
    })
    .append_state(std::move(env), std::move(bindings))
    .map([&](CallGraphData cg, AST ast, TypeGraph tg, Env env, Map<ASTID, std::vector<AsyncValue>> bindings) {
      Binding result;
      std::tie(result, env, bindings) = run_or_assign(
        ex,
        srcs,
        ast,
        tg,
        cg.binding_of,
        to_env_type,
        std::move(env),
        std::move(bindings),
        ASTID{i32(ast.forest.size() - 1)});
      return std::tuple(std::move(result), std::move(ast), std::move(tg), std::move(env), std::move(bindings));
    })
    .map_state([&](AST ast, TypeGraph tg, Env env, Map<ASTID, std::vector<AsyncValue>> bindings) {
      return to_str_bindings(srcs, ast, tg, to_env_type, std::move(env), std::move(bindings));
    })
    .map([](Binding b, Env env, Bindings bindings) {
      assert(b.values.size() <= 1);
      assert(b.values.empty() || env.tg.get<TypeID>(b.type) == type_id(knot::Type<std::string>{}));
      return std::tuple(
        b.values.size() == 1 ? any_cast<std::string>(take(std::move(b.values[0])).wait()) : std::string(),
        std::move(env),
        std::move(bindings));
    })
    .map_error([&](auto errors, Env env, Bindings bindings) {
      return std::tuple(contextualize(srcs, std::move(errors)), std::move(env), std::move(bindings));
    });

  return {"", std::move(env), std::move(str_bindings)};
}

} // namespace ooze
