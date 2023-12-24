#include "pch.h"

#include "ooze/tree.h"
#include "pretty_print.h"
#include "sema.h"
#include "type_check.h"

namespace ooze {

namespace {

struct TypeNameResolution {
  const Env& e;
  std::vector<Slice>* errors;

  Type<TypeID> operator()(const Type<NamedType>& type) {
    return std::visit(
      Overloaded{[&](const NamedType& named) {
                   if(const auto it = e.type_ids.find(named.name); it != e.type_ids.end()) {
                     return Type<TypeID>{it->second, type.ref};
                   } else {
                     errors->push_back(type.ref);
                     return Type<TypeID>{TypeID{}, type.ref};
                   }
                 },
                 [&](const std::vector<Type<NamedType>>& v) {
                   return Type<TypeID>{knot::map<std::vector<Type<TypeID>>>(v, *this), type.ref};
                 },
                 [&](const FunctionType<NamedType>& f) {
                   return Type<TypeID>{knot::map<FunctionType<TypeID>>(f, *this), type.ref};
                 },
                 [&](const FloatingType&) { return floating_type<TypeID>(type.ref); },
                 [&](const BorrowType<NamedType>& b) {
                   return Type<TypeID>{knot::map<BorrowType<TypeID>>(b, *this), type.ref};
                 }},
      type.v);
  }
};

template <typename Typed, typename Untyped>
ContextualResult<Typed> type_name_resolution(const Env& e, const Untyped& u) {
  std::vector<Slice> error_slices;
  auto typed = knot::map<Typed>(u, TypeNameResolution{e, &error_slices});

  std::sort(error_slices.begin(), error_slices.end());

  std::vector<ContextualError> errors;
  std::transform(error_slices.begin(),
                 std::unique(error_slices.begin(), error_slices.end()),
                 std::back_inserter(errors),
                 [](Slice ref) {
                   return ContextualError{ref, "undefined type"};
                 });

  return value_or_errors(std::move(typed), std::move(errors));
}

struct InferBindingCtx {
  std::vector<Set<std::string>> active;
  std::vector<std::pair<std::string, Slice>> args;
};

InferBindingCtx inferred_bindings(InferBindingCtx ctx, const TypedExpr& expr) {
  return std::visit(
    Overloaded{
      [&](const std::vector<TypedExpr>& tuple) { return knot::accumulate(tuple, std::move(ctx), inferred_bindings); },
      [&](const TypedScopeExpr& scope) {
        ctx.active.emplace_back();

        for(const TypedAssignment& assign : scope.assignments) {
          ctx = inferred_bindings(std::move(ctx), *assign.expr);
          knot::preorder(assign.pattern, [&](const ast::Ident& i) { ctx.active.back().insert(i.name); });
        }

        ctx = inferred_bindings(std::move(ctx), *scope.result);

        ctx.active.pop_back();

        return std::move(ctx);
      },
      [&](const TypedSelectExpr& select) {
        return knot::accumulate(select, std::move(ctx), [](auto ctx, const auto& sub_expr) {
          return inferred_bindings(std::move(ctx), *sub_expr);
        });
      },
      [&](const TypedBorrowExpr& borrow) { return inferred_bindings(std::move(ctx), *borrow.expr); },
      [&](const TypedCallExpr& call) {
        return inferred_bindings(inferred_bindings(std::move(ctx), *call.callee), *call.arg);
      },
      [&](const Literal&) { return std::move(ctx); },
      [&](const ast::Ident& ident) {
        if(std::all_of(ctx.active.begin(), ctx.active.end(), [&](const auto& s) {
             return s.find(ident.name) == s.end();
           })) {
          if(std::find_if(ctx.args.begin(), ctx.args.end(), [&](const auto& p) { return p.first == ident.name; }) ==
             ctx.args.end()) {
            ctx.args.emplace_back(ident.name, expr.ref);
          }
        }

        return std::move(ctx);
      }},
    expr.v);
}

struct FunctionConverter {
  const Map<const TypedExpr*, EnvFunctionRef>& overloads;

  CheckedExpr operator()(const TypedExpr& expr) {
    const auto it = overloads.find(&expr);
    return CheckedExpr{
      it != overloads.end() ? it->second : knot::map<ast::ExprVariant<TypeID, EnvFunctionRef>>(expr.v, std::ref(*this)),
      expr.type,
      expr.ref};
  }
};

struct IdentGraphCtx {
  std::vector<std::vector<ASTID>> fanouts;
  std::vector<std::pair<std::string_view, ASTID>> globals;
  std::vector<std::pair<std::string_view, ASTID>> stack;
};

void calculate_ident_graph(IdentGraphCtx& ctx, ASTID id, const SrcMap& sm, const AST& ast) {
  switch(ast.forest[id]) {
  case ASTTag::PatternIdent: ctx.stack.emplace_back(sv(sm, ast.srcs[id.get()]), id); break;
  case ASTTag::Fn:
  case ASTTag::ExprWith: {
    const size_t original_size = ctx.stack.size();
    for(ASTID child : ast.forest.child_ids(id)) {
      calculate_ident_graph(ctx, child, sm, ast);
    }
    ctx.stack.erase(ctx.stack.begin() + original_size, ctx.stack.end());
    break;
  }
  case ASTTag::ExprIdent: {
    const std::string_view ident = sv(sm, ast.srcs[id.get()]);
    const auto it = std::find_if(
      ctx.stack.rbegin(), ctx.stack.rend(), applied([=](std::string_view v, ASTID) { return v == ident; }));
    if(it != ctx.stack.rend()) {
      const auto [sv, pattern_id] = *it;
      ctx.fanouts[id.get()].push_back(pattern_id);
      ctx.fanouts[pattern_id.get()].push_back(id);
    } else {
      for(const auto& [name, pattern_id] : ctx.globals) {
        if(ident == name) {
          ctx.fanouts[id.get()].push_back(pattern_id);
          ctx.fanouts[pattern_id.get()].push_back(id);
        }
      }
    }
    break;
  }
  case ASTTag::Assignment: {
    const auto [pattern, expr] = ast.forest.child_ids(id).take<2>();

    // Parse expr before pattern
    calculate_ident_graph(ctx, expr, sm, ast);
    calculate_ident_graph(ctx, pattern, sm, ast);
    break;
  }
  case ASTTag::Global:
    // Skip identifier of globals, already added up front
    calculate_ident_graph(ctx, ast.forest.child_ids(id).get<1>(), sm, ast);
    break;
  case ASTTag::NativeFn:
  case ASTTag::PatternWildCard:
  case ASTTag::PatternTuple:
  case ASTTag::ExprLiteral:
  case ASTTag::ExprCall:
  case ASTTag::ExprSelect:
  case ASTTag::ExprBorrow:
  case ASTTag::ExprTuple:
    for(ASTID child : ast.forest.child_ids(id)) {
      calculate_ident_graph(ctx, child, sm, ast);
    }
    break;
  };
}

ContextualResult2<CallGraphData, AST, TypeGraph>
create_call_graph_data(const SrcMap& sm, const TypeCache& tc, const Graph<ASTID>& ident_graph, AST ast, TypeGraph tg) {
  Map<ASTID, std::vector<ASTID>> fn_callers;
  Map<ASTID, ASTID> overload_of;

  auto fns = transform_to_vec(ast.forest.root_ids(), [&](ASTID id) { return *ast.forest.first_child(id); });

  const auto unresolved_calls = unique(sorted(knot::accumulate(fns, std::vector<ASTID>{}, [&](auto acc, ASTID ident) {
    return to_vec(ident_graph.fanout(ident), std::move(acc));
  })));

  std::vector<ContextualError2> errors;

  for(ASTID ident : unresolved_calls) {
    const ASTID fn = *ast.forest.first_child(ast.forest.root(ident));
    if(auto [overload, type, matches] = overload_resolution(tc, tg, ident_graph, ast.types, ident); matches == 1) {
      fn_callers[fn].push_back(overload);
      overload_of.emplace(ident, overload);

      if(const auto it = std::lower_bound(fns.begin(), fns.end(), overload); it != fns.end() && *it == overload) {
        fns.erase(it);
      }
    } else if(matches == 0) {
      errors.push_back(
        {ast.srcs[fn.get()],
         "no matching overload found",
         transform_to_vec(
           ident_graph.fanout(fn),
           [&](ASTID id) { return fmt::format("  {}", pretty_print(sm, tg, ast.types[id.get()])); },
           make_vector(fmt::format("deduced {} [{} candidate(s)]",
                                   pretty_print(sm, tg, ast.types[fn.get()]),
                                   ident_graph.fanout(fn).size())))});
    } else {
      errors.push_back(
        {ast.srcs[fn.get()],
         "function call is ambiguous",
         transform_to_vec(
           ident_graph.fanout(fn),
           [&](ASTID id) { return fmt::format("  {}", pretty_print(sm, tg, ast.types[id.get()])); },
           make_vector(
             fmt::format("deduced {} [{} candidate(s)]", pretty_print(sm, tg, ast.types[fn.get()]), matches)))});
    }
  }

  return value_or_errors(CallGraphData{std::move(fn_callers), std::move(overload_of), std::move(fns)},
                         std::move(errors),
                         std::move(ast),
                         std::move(tg));
}

} // namespace

ContextualResult<TypedFunction> type_name_resolution(const Env& e, const UnTypedFunction& f) {
  return type_name_resolution<TypedFunction>(e, std::tie(f.pattern, f.expr));
}

ContextualResult<TypedExpr> type_name_resolution(const Env& e, const UnTypedExpr& expr) {
  return type_name_resolution<TypedExpr>(e, expr);
}

ContextualResult<TypedPattern> type_name_resolution(const Env& e, const UnTypedPattern& pattern) {
  return type_name_resolution<TypedPattern>(e, pattern);
}

ContextualResult<TypedAssignment> type_name_resolution(const Env& e, const UnTypedAssignment& a) {
  return type_name_resolution<TypedAssignment>(e, a);
}

ContextualResult<Type<TypeID>> type_name_resolution(const Env& e, const Type<NamedType>& t) {
  return type_name_resolution<Type<TypeID>>(e, t);
}

ContextualResult2<void, TypeGraph>
type_name_resolution(const SrcMap& sm, const std::unordered_map<std::string, TypeID>& types, TypeGraph tg) {
  std::vector<ContextualError2> errors;

  for(TypeRef t : tg.nodes()) {
    if(tg.get<TypeTag>(t) == TypeTag::Leaf && tg.get<TypeID>(t) == TypeID{}) {
      if(const auto it = types.find(std::string(sv(sm, tg.get<SrcRef>(t)))); it != types.end()) {
        tg.set<TypeID>(t, it->second);
      } else {
        errors.push_back(ContextualError2{tg.get<SrcRef>(t), "undefined type"});
      }
    }
  }

  return void_or_errors(std::move(errors), std::move(tg));
}

Graph<ASTID> calculate_ident_graph(const SrcMap& sm, const AST& ast) {
  IdentGraphCtx ctx = {
    std::vector<std::vector<ASTID>>(ast.forest.size()), transform_filter_to_vec(ast.forest.root_ids(), [&](ASTID id) {
      return ast.forest[id] == ASTTag::Global
               ? std::optional(std::pair(
                   sv(sm, ast.srcs[ast.forest.child_ids(id).get<0>().get()]), ast.forest.child_ids(id).get<0>()))
               : std::nullopt;
    })};

  for(ASTID root : ast.forest.root_ids()) {
    calculate_ident_graph(ctx, root, sm, ast);
  }

  return Graph<ASTID>(ctx.fanouts);
}

TypedPattern inferred_inputs(const TypedExpr& expr, Set<std::string> active) {
  const std::vector<std::pair<std::string, Slice>> args =
    inferred_bindings({make_vector(std::move(active))}, expr).args;
  return {{transform_to_vec(args,
                            [](auto p) {
                              return TypedPattern{ast::Ident{std::move(p.first)}, floating_type<TypeID>(), p.second};
                            })},
          {transform_to_vec(args, [](const auto&) { return floating_type<TypeID>(); })}};
}

ContextualResult<CheckedFunction> overload_resolution(const Env& env, const TypedFunction& f) {
  Map<const TypedExpr*, EnvFunctionRef> all_overloads;

  std::vector<ContextualError> overload_errors;
  for(const TypedExpr* expr : undeclared_bindings(f)) {
    const auto& name = std::get<ast::Ident>(expr->v).name;

    if(const auto it = env.functions.find(name); it == env.functions.end()) {
      overload_errors.push_back({expr->ref, fmt::format("use of undeclared binding '{}'", name)});
    } else {
      std::vector<int> overloads;

      for(size_t i = 0; i < it->second.size(); i++) {
        if(unify_types({it->second[i].type}, expr->type)) {
          overloads.push_back(i);
        }
      }

      if(overloads.size() == 1) {
        all_overloads.emplace(expr, EnvFunctionRef{name, overloads.front()});
      } else if(overloads.empty()) {
        overload_errors.push_back(
          {expr->ref,
           "no matching overload found",
           transform_to_vec(
             it->second,
             [&](const auto& f) { return fmt::format("  {}", pretty_print(env, f.type)); },
             make_vector(
               fmt::format("deduced {} [{} candidate(s)]", pretty_print(env, expr->type), it->second.size())))});
      } else {
        overload_errors.push_back(
          {expr->ref,
           "function call is ambiguous",
           transform_to_vec(
             overloads,
             [&](int i) { return fmt::format("  {}", pretty_print(env, it->second[i].type)); },
             make_vector(
               fmt::format("deduced {} [{} candidate(s)]", pretty_print(env, expr->type), overloads.size())))});
      }
    }
  }

  std::vector<ContextualError> errors =
    !overload_errors.empty() ? std::move(overload_errors) : check_fully_resolved(env, f);

  return errors.empty()
           ? ContextualResult<CheckedFunction>{knot::map<CheckedFunction>(f, FunctionConverter{all_overloads})}
           : Failure{std::move(errors)};
}

ContextualResult2<std::tuple<CallGraphData, Graph<ASTID>>, AST, TypeGraph>
sema(const SrcMap& sm,
     const TypeCache& tc,
     const std::unordered_map<std::string, TypeID>& type_ids,
     const std::unordered_set<TypeID>& copy_types,
     AST ast,
     TypeGraph tg) {
  return type_name_resolution(sm, type_ids, std::move(tg))
    .map_state([&](TypeGraph tg) { return std::tuple(std::move(ast), std::move(tg)); })
    .and_then([&](AST ast, TypeGraph tg) { return apply_language_rules(sm, tc, std::move(ast), std::move(tg)); })
    .map([&](AST ast, TypeGraph tg) {
      auto ig = calculate_ident_graph(sm, ast);
      auto propagations = calculate_propagations(ig, ast.forest);
      return std::tuple(std::tuple(std::move(ig), std::move(propagations)), std::move(ast), std::move(tg));
    })
    .and_then(flattened([&](Graph<ASTID> ig, auto propagations, AST ast, TypeGraph tg) {
      return constraint_propagation(sm, tc, copy_types, ig, propagations, std::move(ast), std::move(tg))
        .map([&](AST ast, TypeGraph tg) {
          return std::tuple(std::tuple(std::move(ig), std::move(propagations)), std::move(ast), std::move(tg));
        });
    }))
    .and_then(flattened([&](Graph<ASTID> ig, auto propagations, AST ast, TypeGraph tg) {
      auto errors = check_fully_resolved(sm, propagations, ast, tg);
      return value_or_errors(std::move(ig), std::move(errors), std::move(ast), std::move(tg));
    }))
    .and_then([&](Graph<ASTID> ig, AST ast, TypeGraph tg) {
      return create_call_graph_data(sm, tc, ig, std::move(ast), std::move(tg))
        .map([&ig](CallGraphData cg, AST ast, TypeGraph tg) {
          return std::tuple(std::tuple(std::move(cg), std::move(ig)), std::move(ast), std::move(tg));
        });
    });
}

} // namespace ooze
