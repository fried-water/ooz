#include "pch.h"

#include "function_graph_construction.h"
#include "function_graph_inner.h"
#include "ooze/tree.h"

#include <numeric>

namespace ooze {

namespace {

std::vector<PassBy> pass_bys_of(const Env& e, const Type<TypeID>& type, std::vector<PassBy> pass_bys = {}) {
  knot::preorder(
    type,
    Overloaded{
      [&](TypeID t) {
        pass_bys.push_back(e.copy_types.find(t) != e.copy_types.end() ? PassBy::Copy : PassBy::Move);
        return false;
      },
      [&](const BorrowType<TypeID>& t) {
        pass_bys.push_back(PassBy::Borrow);
        return false;
      },
      [&](const FunctionType<TypeID>& t) {
        pass_bys.push_back(PassBy::Copy);
        return false;
      },
    });

  return pass_bys;
}

std::vector<bool> borrows_of(const Type<TypeID>& type) {
  std::vector<bool> borrows;

  knot::preorder(
    type,
    Overloaded{
      [&](TypeID t) {
        borrows.push_back(false);
        return false;
      },
      [&](const BorrowType<TypeID>& t) {
        borrows.push_back(true);
        return false;
      },
      [&](const FunctionType<TypeID>& t) {
        borrows.push_back(false);
        return false;
      },
    });

  return borrows;
}

int output_count_of(const Type<TypeID>& type) {
  int count = 0;
  knot::preorder(type,
                 Overloaded{
                   [&](TypeID) { ++count; },
                   [&](const FunctionType<TypeID>&) {
                     ++count;
                     return false;
                   },
                 });

  return count;
}

std::vector<PassBy>
pass_bys_of(const Env& e, const Graph<TypeRef, TypeTag, TypeID>& g, TypeRef t, std::vector<PassBy> pass_bys = {}) {
  preorder(g, t, [&](TypeRef t) {
    switch(g.get<TypeTag>(t)) {
    case TypeTag::Leaf:
      pass_bys.push_back(e.copy_types.find(g.get<TypeID>(t)) != e.copy_types.end() ? PassBy::Copy : PassBy::Move);
      return false;
    case TypeTag::Fn: pass_bys.push_back(PassBy::Copy); return false;
    case TypeTag::Borrow: pass_bys.push_back(PassBy::Borrow); return false;
    case TypeTag::Floating: assert(false);
    case TypeTag::Tuple: return true;
    }
    assert(false);
    return false;
  });

  return pass_bys;
}

std::vector<bool> borrows_of(const Graph<TypeRef, TypeTag, TypeID>& g, const TypeRef& t) {
  std::vector<bool> borrows;

  preorder(g, t, [&](TypeRef t) {
    switch(g.get<TypeTag>(t)) {
    case TypeTag::Leaf:
    case TypeTag::Fn: borrows.push_back(false); return false;
    case TypeTag::Borrow: borrows.push_back(true); return false;
    case TypeTag::Floating: assert(false);
    case TypeTag::Tuple: return true;
    }
    assert(false);
    return false;
  });

  return borrows;
}

int output_count_of(const Graph<TypeRef, TypeTag, TypeID>& g, TypeRef t) {
  int count = 0;
  preorder(g, t, [&](TypeRef t) {
    switch(g.get<TypeTag>(t)) {
    case TypeTag::Leaf:
    case TypeTag::Fn: count++; return false;
    case TypeTag::Borrow:
    case TypeTag::Floating:
    case TypeTag::Tuple: return true;
    }
    assert(false);
    return false;
  });

  return count;
}

struct GraphContext {
  ConstructingGraph cg;
  std::vector<Map<std::string, std::vector<Oterm>>> bindings;
};

GraphContext append_bindings(const TypedPattern& pattern, const std::vector<Oterm>& terms, GraphContext ctx) {
  int i = 0;
  co_visit(pattern,
           pattern.type,
           Overloaded{[&](const auto&, const auto& type, const ast::Ident& ident, const auto&) {
                        ctx.bindings.back()[ident.name] =
                          knot::preorder_accumulate(type, std::vector<Oterm>{}, [&](auto v, TypeID) {
                            v.push_back(terms[i++]);
                            return v;
                          });
                      },
                      [&](const ast::WildCard&, const auto& type) { knot::preorder(type, [&](TypeID) { i++; }); }});
  return ctx;
}

std::pair<GraphContext, std::vector<Oterm>> add_expr(const Env&, const CheckedExpr&, GraphContext);

std::pair<GraphContext, std::vector<Oterm>>
add_expr(const Env& e, const std::vector<CheckedExpr>& exprs, const Type<TypeID>&, GraphContext ctx) {
  return knot::accumulate(
    exprs, std::pair(std::move(ctx), std::vector<Oterm>{}), [&](auto pair, const CheckedExpr& expr) {
      auto [ctx, terms] = add_expr(e, expr, std::move(pair.first));
      return std::pair(std::move(ctx), to_vec(std::move(terms), std::move(pair.second)));
    });
}

std::pair<GraphContext, std::vector<Oterm>>
add_expr(const Env& e, const ast::ScopeExpr<TypeID, EnvFunctionRef>& scope, const Type<TypeID>&, GraphContext ctx) {
  ctx.bindings.emplace_back();

  ctx = knot::accumulate(scope.assignments, std::move(ctx), [&](GraphContext ctx, const CheckedAssignment& assignment) {
    std::vector<Oterm> terms;
    std::tie(ctx, terms) = add_expr(e, *assignment.expr, std::move(ctx));
    return append_bindings(assignment.pattern, terms, std::move(ctx));
  });

  std::vector<Oterm> terms;
  std::tie(ctx, terms) = add_expr(e, *scope.result, std::move(ctx));
  ctx.bindings.pop_back();

  return {std::move(ctx), std::move(terms)};
}

std::pair<GraphContext, std::vector<Oterm>> add_expr(
  const Env& e, const ast::SelectExpr<TypeID, EnvFunctionRef>& select, const Type<TypeID>& type, GraphContext ctx) {
  std::vector<Oterm> cond_terms;
  std::vector<Oterm> if_terms;
  std::vector<Oterm> else_terms;

  std::tie(ctx, cond_terms) = add_expr(e, *select.condition, std::move(ctx));
  std::tie(ctx, if_terms) = add_expr(e, *select.if_expr, std::move(ctx));
  std::tie(ctx, else_terms) = add_expr(e, *select.else_expr, std::move(ctx));

  assert(cond_terms.size() == 1);
  assert(if_terms.size() == else_terms.size());

  std::vector<PassBy> pass_bys;
  pass_bys.reserve(cond_terms.size() + if_terms.size() + else_terms.size());
  pass_bys = pass_bys_of(e, select.condition->type, std::move(pass_bys));
  pass_bys = pass_bys_of(e, select.if_expr->type, std::move(pass_bys));
  pass_bys = pass_bys_of(e, select.else_expr->type, std::move(pass_bys));

  std::vector<Oterm> terms =
    ctx.cg.add(create_async_select(),
               flatten(std::move(cond_terms), std::move(if_terms), std::move(else_terms)),
               pass_bys,
               output_count_of(type));
  return {std::move(ctx), std::move(terms)};
}

std::pair<GraphContext, std::vector<Oterm>>
add_expr(const Env& e, const ast::CallExpr<TypeID, EnvFunctionRef>& call, const Type<TypeID>& type, GraphContext ctx) {
  std::vector<Oterm> arg_terms;
  std::tie(ctx, arg_terms) = add_expr(e, *call.arg, std::move(ctx));

  const int output_count = output_count_of(type);

  if(const auto* ref = std::get_if<EnvFunctionRef>(&call.callee->v); ref) {
    const auto it = e.functions.find(ref->name);
    assert(it != e.functions.end() && ref->overload_idx < it->second.size());
    const EnvFunction& ef = it->second[ref->overload_idx];

    std::vector<Oterm> terms = std::visit(
      Overloaded{
        [&](const AsyncFn& f) { return ctx.cg.add(f, arg_terms, pass_bys_of(e, call.arg->type), output_count); },
        [&](const FunctionGraph& f) { return ctx.cg.add(f, arg_terms); },
        [&](const TypedFunction&) {
          const auto it = find_if(ef.instatiations, [&](const auto& p) {
            return std::get<FunctionType<TypeID>>(call.callee->type.v) == p.first;
          });
          assert(it != ef.instatiations.end());
          return ctx.cg.add(it->second, arg_terms);
        }},
      ef.f);
    return {std::move(ctx), std::move(terms)};
  } else {
    auto [ctx2, callee_terms] = add_expr(e, *call.callee, std::move(ctx));
    assert(callee_terms.size() == 1);

    std::vector<PassBy> pass_bys;
    pass_bys.reserve(callee_terms.size() + arg_terms.size());
    pass_bys = pass_bys_of(e, call.callee->type, std::move(pass_bys));
    pass_bys = pass_bys_of(e, call.arg->type, std::move(pass_bys));

    std::vector<Oterm> terms =
      ctx2.cg.add(create_async_functional(output_count),
                  flatten(std::move(callee_terms), std::move(arg_terms)),
                  pass_bys,
                  output_count);
    return {std::move(ctx2), std::move(terms)};
  }
}

std::pair<GraphContext, std::vector<Oterm>>
add_expr(const Env& e, const ast::BorrowExpr<TypeID, EnvFunctionRef>& borrow, const Type<TypeID>&, GraphContext ctx) {
  return add_expr(e, *borrow.expr, std::move(ctx));
}

std::pair<GraphContext, std::vector<Oterm>>
add_expr(const Env& e, const ast::Ident& ident, const Type<TypeID>& type, GraphContext ctx) {
  std::optional<std::vector<Oterm>> terms = std::accumulate(
    ctx.bindings.rbegin(),
    ctx.bindings.rend(),
    std::optional<std::vector<Oterm>>{},
    [&](auto acc, const auto& bindings) {
      if(acc) {
        return acc;
      } else {
        const auto it = bindings.find(ident.name);
        return it != bindings.end() ? std::optional(it->second) : std::nullopt;
      }
    });

  return {std::move(ctx), std::move(*terms)};
}

std::pair<GraphContext, std::vector<Oterm>>
add_expr(const Env&, const Literal& literal, const Type<TypeID>&, GraphContext ctx) {
  std::vector<Oterm> terms =
    std::visit([&](const auto& value) { return ctx.cg.add(create_async_value(Any(value)), {}, {}, 1); }, literal);
  return {std::move(ctx), std::move(terms)};
}

std::pair<GraphContext, std::vector<Oterm>>
add_expr(const Env& e, const EnvFunctionRef& fn_ref, const Type<TypeID>& type, GraphContext ctx) {
  const EnvFunction& ef = e.functions.at(fn_ref.name)[fn_ref.overload_idx];

  AsyncFn f = std::visit(
    Overloaded{[&](const AsyncFn& f) { return f; },
               [](const FunctionGraph& f) { return create_async_graph(f); },
               [&](const TypedFunction&) {
                 const auto it = find_if(ef.instatiations, [&](const auto& p) {
                   return std::get<FunctionType<TypeID>>(type.v) == p.first;
                 });
                 assert(it != ef.instatiations.end());
                 return create_async_graph(it->second);
               }},
    ef.f);

  std::vector<Oterm> terms = ctx.cg.add(create_async_value(Any(std::move(f))), {}, {}, 1);

  return {std::move(ctx), std::move(terms)};
}

std::pair<GraphContext, std::vector<Oterm>> add_expr(const Env& e, const CheckedExpr& expr, GraphContext ctx) {
  return std::visit([&](const auto& sub_expr) { return add_expr(e, sub_expr, expr.type, std::move(ctx)); }, expr.v);
}

struct GraphContext2 {
  ConstructingGraph cg;
  Map<ASTID, std::vector<Oterm>> bindings;
};

GraphContext2
append_bindings(const AST& ast, const Types& types, ASTID pattern, const std::vector<Oterm>& terms, GraphContext2 ctx) {
  const auto count_terms = [&](auto self, TypeRef t) -> i32 {
    switch(types.graph.get<TypeTag>(t)) {
    case TypeTag::Tuple:
    case TypeTag::Borrow:
      return knot::accumulate(types.graph.fanout(t), 0, [=](i32 acc, TypeRef t) { return acc + self(self, t); });
    case TypeTag::Floating:
    case TypeTag::Leaf:
    case TypeTag::Fn: return 1;
    }
    return 0;
  };

  auto it = terms.begin();
  for(ASTID id : ast.forest.pre_order_ids(pattern)) {
    if(ast.forest[id] == ASTTag::PatternWildCard) {
      it += count_terms(count_terms, types.ast_types[id.get()]);
    } else if(ast.forest[id] == ASTTag::PatternTuple) {
      const auto count = count_terms(count_terms, types.ast_types[id.get()]);
      ctx.bindings[id] = std::vector<Oterm>(it, it + count);
      it += count;
    }
  }
  return ctx;
}

std::pair<GraphContext2, std::vector<Oterm>>
add_expr(const Env&,
         const AST&,
         const Types& types,
         const Map<ASTID, EnvFunctionRef>& fns,
         const Graph<ASTID>& ident_graph,
         ASTID,
         GraphContext2);

std::pair<GraphContext2, std::vector<Oterm>> add_select_expr(
  const Env& e,
  const AST& ast,
  const Types& types,
  const Map<ASTID, EnvFunctionRef>& fns,
  const Graph<ASTID>& ident_graph,
  ASTID id,
  GraphContext2 ctx) {
  std::vector<Oterm> cond_terms;
  std::vector<Oterm> if_terms;
  std::vector<Oterm> else_terms;

  const auto [cond_id, if_id, else_id] = ast.forest.child_ids(id).take<3>();

  std::tie(ctx, cond_terms) = add_expr(e, ast, types, fns, ident_graph, cond_id, std::move(ctx));
  std::tie(ctx, if_terms) = add_expr(e, ast, types, fns, ident_graph, if_id, std::move(ctx));
  std::tie(ctx, else_terms) = add_expr(e, ast, types, fns, ident_graph, else_id, std::move(ctx));

  assert(cond_terms.size() == 1);
  assert(if_terms.size() == else_terms.size());

  std::vector<PassBy> pass_bys;
  pass_bys.reserve(cond_terms.size() + if_terms.size() + else_terms.size());
  pass_bys = pass_bys_of(e, types.graph, types.ast_types[cond_id.get()], std::move(pass_bys));
  pass_bys = pass_bys_of(e, types.graph, types.ast_types[if_id.get()], std::move(pass_bys));
  pass_bys = pass_bys_of(e, types.graph, types.ast_types[else_id.get()], std::move(pass_bys));

  std::vector<Oterm> terms =
    ctx.cg.add(create_async_select(),
               flatten(std::move(cond_terms), std::move(if_terms), std::move(else_terms)),
               pass_bys,
               output_count_of(types.graph, types.ast_types[id.get()]));
  return {std::move(ctx), std::move(terms)};
}

std::pair<GraphContext2, std::vector<Oterm>> add_call_expr(
  const Env& e,
  const AST& ast,
  const Types& types,
  const Map<ASTID, EnvFunctionRef>& fns,
  const Graph<ASTID>& ident_graph,
  ASTID id,
  GraphContext2 ctx) {
  std::vector<Oterm> callee_terms;
  std::vector<Oterm> arg_terms;

  const auto [callee, arg] = ast.forest.child_ids(id).take<2>();

  // TODO optimize when arg is EnvFunctionRef, or have some function inline step?
  std::tie(ctx, callee_terms) = add_expr(e, ast, types, fns, ident_graph, callee, std::move(ctx));
  std::tie(ctx, arg_terms) = add_expr(e, ast, types, fns, ident_graph, arg, std::move(ctx));

  assert(callee_terms.size() == 1);
  arg_terms.insert(arg_terms.begin(), callee_terms.front());

  std::vector<PassBy> pass_bys;
  pass_bys.reserve(arg_terms.size());
  pass_bys = pass_bys_of(e, types.graph, types.ast_types[callee.get()], std::move(pass_bys));
  pass_bys = pass_bys_of(e, types.graph, types.ast_types[arg.get()], std::move(pass_bys));

  const int output_count = output_count_of(types.graph, types.ast_types[id.get()]);
  std::vector<Oterm> terms = ctx.cg.add(create_async_functional(output_count), arg_terms, pass_bys, output_count);
  return {std::move(ctx), std::move(terms)};
}

std::pair<GraphContext2, std::vector<Oterm>>
add_expr(const Env& e,
         const AST& ast,
         const Types& types,
         const Map<ASTID, EnvFunctionRef>& fns,
         const Graph<ASTID>& ident_graph,
         ASTID id,
         GraphContext2 ctx) {
  switch(ast.forest[id]) {
  case ASTTag::PatternWildCard:
  case ASTTag::PatternIdent:
  case ASTTag::PatternTuple:
  case ASTTag::Fn:
  case ASTTag::Assignment:
  case ASTTag::RootFn: assert(false); return {};
  case ASTTag::ExprLiteral: {
    std::vector<Oterm> terms = std::visit(
      [&](const auto& v) { return ctx.cg.add(create_async_value(Any(v)), {}, {}, 1); }, lookup_literal(ast, id));
    return {std::move(ctx), std::move(terms)};
  }
  case ASTTag::ExprCall: assert(false);
  case ASTTag::ExprSelect: return add_select_expr(e, ast, types, fns, ident_graph, id, std::move(ctx));
  case ASTTag::ExprBorrow:
    return add_expr(e, ast, types, fns, ident_graph, *ast.forest.first_child(id), std::move(ctx));
  case ASTTag::ExprWith: {
    const auto [assignment, expr] = ast.forest.child_ids(id).take<2>();
    const auto [pattern, assign_expr] = ast.forest.child_ids(assignment).take<2>();

    auto [assign_ctx, assign_terms] = add_expr(e, ast, types, fns, ident_graph, assign_expr, std::move(ctx));
    ctx = append_bindings(ast, types, pattern, assign_terms, std::move(assign_ctx));

    return add_expr(e, ast, types, fns, ident_graph, expr, std::move(ctx));
  }
  case ASTTag::ExprTuple:
    return knot::accumulate(
      ast.forest.child_ids(id), std::pair(std::move(ctx), std::vector<Oterm>{}), [&](auto pair, ASTTag tuple_element) {
        auto [ctx, terms] = add_expr(e, ast, types, fns, ident_graph, tuple_element, std::move(pair.first));
        return std::pair(std::move(ctx), to_vec(std::move(terms), std::move(pair.second)));
      });
  case ASTTag::ExprIdent: {
    if(const auto fn_it = fns.find(id); fn_it != fns.end()) {
      const EnvFunctionRef fn_ref = fn_it->second;
      const EnvFunction& ef = e.functions.at(fn_ref.name)[fn_ref.overload_idx];

      AsyncFn f = std::visit(
        Overloaded{[&](const AsyncFn& f) { return f; },
                   [](const FunctionGraph& f) { return create_async_graph(f); },
                   [&](const TypedFunction&) {
                     // TODO look up instantiation via ast type
                     assert(false);
                     return create_async_value(Any(0));
                   }},
        ef.f);

      std::vector<Oterm> terms = ctx.cg.add(create_async_value(Any(std::move(f))), {}, {}, 1);

      return {std::move(ctx), std::move(terms)};
    } else {
      assert(ident_graph.num_fanout(id) == 1);
      std::vector<Oterm> terms = ctx.bindings.at(ident_graph.fanout(id)[0]);
      return {std::move(ctx), std::move(terms)};
    }
  }
  }
  return {};
}

} // namespace

FunctionGraph create_graph(const Env& e, const CheckedFunction& f) {
  auto [cg, terms] = make_graph(borrows_of(f.pattern.type));
  auto [ctx, output_terms] = add_expr(e, f.expr, append_bindings(f.pattern, terms, GraphContext{std::move(cg), {{}}}));
  return std::move(ctx.cg).finalize(output_terms, pass_bys_of(e, f.expr.type));
}

FunctionGraph create_graph(const Env& e,
                           const AST& ast,
                           const Types& types,
                           const Map<ASTID, EnvFunctionRef>& fns,
                           const Graph<ASTID>& ident_graph,
                           ASTID fn_id) {
  assert(ast.forest[fn_id] == ASTTag::Fn);

  const auto [pattern, expr] = ast.forest.child_ids(fn_id).take<2>();

  auto [cg, terms] = make_graph(borrows_of(types.graph, types.ast_types[pattern.get()]));
  auto [ctx, output_terms] = add_expr(
    e, ast, types, fns, ident_graph, expr, append_bindings(ast, types, pattern, terms, GraphContext2{std::move(cg)}));
  return std::move(ctx.cg).finalize(output_terms, pass_bys_of(e, types.graph, types.ast_types[expr.get()]));
}

} // namespace ooze
