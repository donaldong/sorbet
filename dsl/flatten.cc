#include "dsl/flatten.h"
#include "ast/Helpers.h"
#include "ast/ast.h"
#include "ast/treemap/treemap.h"
#include "core/core.h"

#include <utility>

using namespace std;

// This DSL pass flattens nested methods, so that once we've reached a non-definition AST node (i.e. not a ClassDef or a
// MethodDef) then we know that there are no MethodDefs lurking deeper in the tree. In order to work correctly, this
// also needs to move some non-method-def things as well, specifically `sig`s and sends for method visibility
// (e.g. `private` and the like), and it also updates the static-ness of some MethodDefs based on where they have
// appeared in a nested context.
//
// So, a file like the following
//
// class A
//   sig{void}
//   private def foo
//     sig{void}
//     def self.bar; end
//   end
// end
//
// will morally be transformed into the following
//
// class A
//   sig{void}
//   private def foo; end
//   sig{void}
//   def bar; end   # notice the lack of `self.` here
// end
//
// So no nested methods exist any longer, and additionally, the nested method `bar` has had the `self.` qualifier
// removed: if you run the above code in Ruby, you'll find that `bar` is not defined as a class method on `A`, but
// rather as a not-always-available instance method on `A`, so introducing it as a static method is not at all
// correct.
//
// It does this by maintaining a stack of indices and state and a queue of expressions during a tree traversal. Every
// time something which might concievably need to be moved is found (i.e. a method definition or a send) we reserve
// space for it in a queue and then add metadata about it---the intended queue slot as well as whether it is a class
// method---to the stack. We can use the stack to disambiguate nested methods and also determine method context. Once
// our tree traversal has left that subtree, we can safely move that subtree into the queue and replace it with an
// EmptyTree. Once we leave a class scope, we empty that entire queue into the class scope, as well.
//
// The logic used to determine what sends need to be moved is purely syntactic, which suggests that if someone were to
// redefine the method `private` and apply it to a `MethodDef`, then it will get caught by this and moved. This seems
// vanishingly unlikely and would probably break a lot of other things, as well.
namespace sorbet::dsl {

class FlattenWalk {
private:
public:
    FlattenWalk() {
        newMethodSet();
    }
    ~FlattenWalk() {
        ENFORCE(methodScopes.empty());
    }

    unique_ptr<ast::ClassDef> preTransformClassDef(core::Context ctx, unique_ptr<ast::ClassDef> classDef) {
        newMethodSet();
        return classDef;
    }

    unique_ptr<ast::MethodDef> preTransformMethodDef(core::Context ctx, unique_ptr<ast::MethodDef> methodDef) {
        auto &methods = curMethodSet();
        if (skipMethods.contains(methodDef.get())) {
            ENFORCE(!methods.stack.empty());
            return methodDef;
        }
        int staticLevel = computeStaticLevel(methodDef.get());

        // we should only move methods that /are/ nested, so if the method stack is empty, then don't bother adding
        // space in the move queue and record the index as -1
        if (methods.stack.size() == 0) {
            methods.stack.emplace_back(-1, staticLevel);
        } else {
            methods.stack.emplace_back(methods.methods.size(), staticLevel);
            methods.methods.emplace_back();
        }

        return methodDef;
    }

    // Returns `true` if the method is one of the modifier names in Ruby (e.g. 'private' or 'protected' or
    // similar). This does not need to know about `module_function` because we have already re-written it in a previous
    // DSL pass.
    bool isMethodModifier(ast::Send &send) {
        auto fun = send.fun;
        return (fun == core::Names::private_() || fun == core::Names::protected_() || fun == core::Names::public_() ||
                fun == core::Names::privateClassMethod()) &&
               send.args.size() == 1 && ast::isa_tree<ast::MethodDef>(send.args[0].get());
    }

    // We might want to move sends as well: either if they're method modifiers like `private` or `protected` or if
    // they're `sig`s. If so, then we'll treat them like we treat methods on our method stack
    unique_ptr<ast::Send> preTransformSend(core::Context ctx, unique_ptr<ast::Send> send) {
        if (send->fun == core::Names::sig() || isMethodModifier(*send)) {
            auto &methods = curMethodSet();
            int staticLevel = 0;
            if (isMethodModifier(*send) && send->args.size() >= 1) {
                // if this is a method modifier like `private` or `protected`, then we don't need to add a new scope
                // when we traverse the method itself, so add it to the ignore set...
                skipMethods.insert(send->args[0].get());
                auto methodDef = ast::cast_tree<ast::MethodDef>(send->args.front().get());
                ENFORCE(methodDef != nullptr);
                staticLevel = computeStaticLevel(methodDef);
            }

            // we should only move methods that are nested, so if the method stack is empty, then don't bother adding
            // space in the move queue and record the index as -1
            if (methods.stack.size() == 0) {
                methods.stack.emplace_back(-1, staticLevel);
            } else {
                methods.stack.emplace_back(methods.methods.size(), staticLevel);
                methods.methods.emplace_back();
            }
        }
        return send;
    }

    unique_ptr<ast::Expression> postTransformSend(core::Context ctx, unique_ptr<ast::Send> send) {
        if (send->fun == core::Names::sig() || isMethodModifier(*send)) {
            auto &methods = curMethodSet();
            ENFORCE(!methods.stack.empty());

            // we may not need to move this method at all: check first
            if (methods.stack.back().idx == -1) {
                methods.stack.pop_back();
                return send;
            }

            // otherwise, we have a spot in the queue for it that has not yet been filled in
            ENFORCE(methods.methods.size() > methods.stack.back().idx);
            ENFORCE(methods.methods[methods.stack.back().idx].expr == nullptr);

            methods.methods[methods.stack.back().idx] = {move(send), methods.stack.back().staticLevel};
            methods.stack.pop_back();

            return make_unique<ast::EmptyTree>();
        } else {
            return send;
        }
    }

    unique_ptr<ast::Expression> postTransformClassDef(core::Context ctx, unique_ptr<ast::ClassDef> classDef) {
        classDef->rhs = addMethods(ctx, std::move(classDef->rhs), classDef->loc);
        return classDef;
    };

    unique_ptr<ast::Expression> postTransformMethodDef(core::Context ctx, unique_ptr<ast::MethodDef> methodDef) {
        // if this method is contained in a send like `private` or `protected`, then we should not move it, because
        // moving the send will do that for us
        if (skipMethods.contains(methodDef.get())) {
            return methodDef;
        }
        auto &methods = curMethodSet();
        ENFORCE(!methods.stack.empty());

        // we may not need to move this method at all: check first
        if (methods.stack.back().idx == -1) {
            methods.stack.pop_back();
            return methodDef;
        }

        ENFORCE(methods.methods.size() > methods.stack.back().idx);
        ENFORCE(methods.methods[methods.stack.back().idx].expr == nullptr);

        methods.methods[methods.stack.back().idx] = {std::move(methodDef), methods.stack.back().staticLevel};
        methods.stack.pop_back();
        return make_unique<ast::EmptyTree>();
    };

    unique_ptr<ast::Expression> addMethods(core::Context ctx, unique_ptr<ast::Expression> tree) {
        auto &methods = curMethodSet().methods;
        if (methods.empty()) {
            ENFORCE(popCurMethodDefs().empty());
            return tree;
        }
        if (methods.size() == 1 && ast::isa_tree<ast::EmptyTree>(tree.get())) {
            // It was only 1 method to begin with, put it back
            unique_ptr<ast::Expression> methodDef = std::move(popCurMethodDefs()[0].expr);
            return methodDef;
        }

        auto insSeq = ast::cast_tree<ast::InsSeq>(tree.get());
        if (insSeq == nullptr) {
            ast::InsSeq::STATS_store stats;
            tree = make_unique<ast::InsSeq>(tree->loc, std::move(stats), std::move(tree));
            return addMethods(ctx, std::move(tree));
        }

        for (auto &method : popCurMethodDefs()) {
            ENFORCE(method.expr != nullptr);
            insSeq->stats.emplace_back(std::move(method.expr));
        }
        return tree;
    }

private:
    ast::ClassDef::RHS_store addMethods(core::Context ctx, ast::ClassDef::RHS_store rhs, core::Loc loc) {
        if (curMethodSet().methods.size() == 1 && rhs.size() == 1 && ast::isa_tree<ast::EmptyTree>(rhs[0].get())) {
            // It was only 1 method to begin with, put it back
            rhs.pop_back();
            rhs.emplace_back(std::move(popCurMethodDefs()[0].expr));
            return rhs;
        }

        auto exprs = popCurMethodDefs();
        // TODO: remove all this
        //
        // this add the nested methods at the appropriate 'staticness level'

        int highestLevel = 0;
        for (int i = 0; i < exprs.size(); i++) {
            auto &expr = exprs[i];
            if (highestLevel < expr.staticLevel) {
                highestLevel = expr.staticLevel;
            }
            // we need to make sure that we keep sends with their attached methods, so fix that up here
            if (i > 0) {
                auto send = ast::cast_tree<ast::Send>(exprs[i - 1].expr.get());
                if (send != nullptr && send->fun == core::Names::sig()) {
                    exprs[i - 1].staticLevel = expr.staticLevel;
                }
            }
        }

        // these will store the bodies of the `class << self` blocks we create at the end
        vector<ast::ClassDef::RHS_store> nestedBlocks;
        for (int level = 2; level <= highestLevel; level++) {
            nestedBlocks.emplace_back();
        }

        // this vector contains all the possible RHS target locations that we might move to
        vector<ast::ClassDef::RHS_store*> targets;
        // 0 and 1 both go into the class itself
        targets.emplace_back(&rhs);
        targets.emplace_back(&rhs);
        // 2 and up go into the to-be-created `class << self` blocks
        for (auto &tgt : nestedBlocks) {
            targets.emplace_back(&tgt);
        }

        // move everything to its appropriate target
        for (auto &expr : exprs) {
            if (auto methodDef = ast::cast_tree<ast::MethodDef>(expr.expr.get())) {
                methodDef->setIsSelf(expr.staticLevel > 0);
            }
            targets[expr.staticLevel]->emplace_back(std::move(expr.expr));
        }

        // generate the nested `class << self` blocks as needed and add them to the class
        for (auto &body : nestedBlocks) {
            auto classDef =
                ast::MK::Class(loc, loc,
                               make_unique<ast::UnresolvedIdent>(core::Loc::none(), ast::UnresolvedIdent::Class,
                                                                 core::Names::singleton()),
                               {}, std::move(body), ast::ClassDefKind::Class);
            rhs.emplace_back(std::move(classDef));
        }

        return rhs;
    }

    int computeStaticLevel(ast::MethodDef *methodDef) {
        auto &methods = curMethodSet();
        int prevLevel = methods.stack.size() > 0 ? methods.stack.back().staticLevel : 0;
        return prevLevel + (methodDef->isSelf() ? 1 : 0);
    }

    struct MethodData {
        // This is an index into the methods stack
        int idx;
        // we need to keep information around about whether we're in a static outer context: for example, if we have
        //
        //   def self.foo; def bar; end; end
        //
        // then we should flatten it to
        //
        //  def self.foo; end
        //  def self.bar; end
        //
        // which means when we get to `bar` we need to know that the outer context `foo` is static. We pass that down
        // the current stack by means of this `isStatic` variable.
        int staticLevel;
        MethodData(int idx, int staticLevel) : idx(idx), staticLevel(staticLevel){};
    };
    struct MovedItem {
        unique_ptr<ast::Expression> expr;
        int staticLevel;
        MovedItem(unique_ptr<ast::Expression> expr, int staticLevel) : expr(move(expr)), staticLevel(staticLevel){};
        MovedItem() = default;
    };
    struct Methods {
        vector<MovedItem> methods;
        vector<MethodData> stack;
        Methods() = default;
    };
    void newMethodSet() {
        methodScopes.emplace_back();
    }
    Methods &curMethodSet() {
        ENFORCE(!methodScopes.empty());
        return methodScopes.back();
    }
    void popCurMethodSet() {
        ENFORCE(!methodScopes.empty());
        methodScopes.pop_back();
    }

    vector<MovedItem> popCurMethodDefs() {
        auto ret = std::move(curMethodSet().methods);
        ENFORCE(curMethodSet().stack.empty());
        popCurMethodSet();
        return ret;
    };

    // We flatten methods so that we have an arbitrary hierarchy of classes each of which has a flat list of
    // methods. This prevents methods from existing deeper inside the hierarchy, enabling later traversals to stop
    // recursing over the AST once they've reached a method def.
    vector<Methods> methodScopes;
    // this allows us to skip adding methods to the method stack if we are going to add them as part of a larger
    // expression: for example, if we have already seen the send `private(def foo...)` then we'll add the entire send,
    // and not just the method.
    UnorderedSet<ast::Expression *> skipMethods;
};

unique_ptr<ast::Expression> Flatten::run(core::Context ctx, unique_ptr<ast::Expression> tree) {
    FlattenWalk flatten;
    tree = ast::TreeMap::apply(ctx, flatten, std::move(tree));
    tree = flatten.addMethods(ctx, std::move(tree));

    return tree;
}

} // namespace sorbet::dsl
