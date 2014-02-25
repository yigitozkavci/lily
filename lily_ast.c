#include "lily_ast.h"
#include "lily_impl.h"

/** How ast works
    * Every tree represents something, either an operation, or a value. Some
      trees make use of some of the trees inside of them. As an example, binary
      uses left, right, and parent to refer to the left of the expression, the
      right of it, and the expression that is above it.
    * Evaluation is done from the bottom up, and left to right. Parents are
      higher up in the tree, and go later.
    * Example: 5 + 6 * 7
          5        +        6        *       7
      ---------------------------------------------
      |   5    |  +     |  +     |  +    |  +     |
      |        | /      | / \    | / \   | / \    |
      |        |5       |5   6   |5   *  |5   *   |
      |        |        |        |   /   |   / \  |
      |        |        |        |  6    |  6   7 |
      ---------------------------------------------
    * The ast pool keeps track of two important trees: the active and the top.
      The active tree is what new values will be given to. In the above example,
      5 is initially current until + is added. * becomes current when it is
      added. The top tree is the tree that is the parent of all other trees.
      This is given by the parser to the emitter so the emitter can recurse
      through all trees.
    * The pool stores the asts used for an expression, and reuses those on the
      next pass. The trees are linked to each other through the ->next_tree
      field. New trees are added as they are needed, so there is no waste.

    ast is responsible for:
    * Providing an API to parser for merging in trees. Parser is expected to
      use this right (ex: not giving 3 values to a binary op, etc.), so there is
      no extra checking in ast.
    * Keeping track of the depth of the current expression (how many calls/
      parent ops/lists there are). This is used by parser to guard against an
      expression ending early (save_index is 0 if there are no subtrees).
**/

/** ast pool init, deletion, and reset. **/
lily_ast_pool *lily_new_ast_pool(lily_raiser *raiser, int pool_size)
{
    lily_ast_pool *ap = lily_malloc(sizeof(lily_ast_pool));
    int i, ok = 1;
    lily_ast *last_tree;

    if (ap == NULL)
        return NULL;

    ap->raiser = raiser;
    ap->saved_trees = lily_malloc(sizeof(lily_ast *) * pool_size);
    ap->active = NULL;
    ap->root = NULL;
    ap->save_index = 0;
    ap->save_size = pool_size;
    ap->available_start = NULL;
    ap->available_current = NULL;

    last_tree = NULL;
    for (i = 0;i < pool_size;i++) {
        lily_ast *new_tree = lily_malloc(sizeof(lily_ast));
        if (new_tree == NULL) {
            ok = 0;
            break;
        }

        new_tree->next_tree = last_tree;
        last_tree = new_tree;
    }

    ap->available_start = last_tree;
    ap->available_current = last_tree;

    if (ap->saved_trees == NULL || ok == 0) {
        lily_free_ast_pool(ap);
        return NULL;
    }

    return ap;
}

void lily_free_ast_pool(lily_ast_pool *ap)
{
    lily_ast *ast_iter = ap->available_start;
    lily_ast *ast_temp;

    while (ast_iter) {
        ast_temp = ast_iter->next_tree;

        lily_free(ast_iter);

        ast_iter = ast_temp;
    }

    lily_free(ap->saved_trees);
    lily_free(ap);
}

/* lily_ast_reset_pool
   This sets the trees in the pool as not being used so that they can be used
   again. */
void lily_ast_reset_pool(lily_ast_pool *ap)
{
    ap->root = NULL;
    ap->active = NULL;
    ap->available_current = ap->available_start;
}

/** Common private merging functions **/
/* merge_absorb
   This handles a merge wherein the active tree is taken over by the new ast
   as an argument. This was originally done to handle turning things like
   a.concat("b") into concat(a, "b"), but it also works for list build,
   subscripts, and much more. */
static void merge_absorb(lily_ast_pool *ap, lily_ast *active, lily_ast *new_ast)
{
    lily_ast *target;

    if (active->tree_type < tree_typecast) {
        /* For non-binary/typecast trees, swallow the current tree as an
           'argument', and become the new current tree. */
        if (ap->root == active)
            ap->root = new_ast;

        /* The call becomes active because it's taking over the var. Otherwise,
           lily_ast_enter_call will think the var is the parent, and make the
           var the current when the call is done. That's bad. */
        ap->active = new_ast;
        target = active;
    }
    else {
        /* This gets called when the merge is against the rhs of a binary or the
           rhs of a typecast.
           Ex: 'a = b.concat("c") < b.concat("d")
               '@(type: value[0])'   ^
                             ^

           This is always against the rhs, like how values always add to the
           rhs of a binary op. This cannot become current or root, because the
           binary always has priority over it. */
        target = active->right;
        active->right = new_ast;
    }

    new_ast->arg_start = target;
    new_ast->arg_top = target;
    new_ast->args_collected = 1;
    new_ast->next_arg = NULL;
}

/* merge_unary
   This handles a unary merge wherein 'active' is ap->active and new_ast is the
   tree to be merged in. */
static void merge_unary(lily_ast_pool *ap, lily_ast *new_active, lily_ast *new_ast)
{
    lily_ast *active = new_active;
    /* 'a = ' or '@(type: ', so there's no value for the right side...yet. */
    if (active->tree_type >= tree_typecast && active->right == NULL)
        active->right = new_ast;
    else {
        /* Might be 'a = -' or '@(type: ', so there's already at least 1
           unary value. */
        if (active->tree_type >= tree_typecast)
            active = active->right;

        /* Unary ops are right->left (opposite of binary), and all have the same
           precedence. So values, calls, and even other unary ops will have to
           walk down to become the child of the lowest unary op.
           The final condition ensures that we get the lowest unary tree,
           instead of the lowest unary tree's value. This is important, because
           that lowest unary tree may need to be updated. */
        while (active->tree_type == tree_unary && active->left != NULL &&
               active->left->tree_type == tree_unary)
            active = active->left;

        if (active->left == NULL)
            active->left = new_ast;
        else if (new_ast->tree_type == tree_subscript) {
            /* Subscript is a special case because it comes after a var and
               swallows it as the first arg. */
            merge_absorb(ap, active->left, new_ast);
            /* new_ast contains the tree in active->left, so update the unary
               tree... */
            active->left = new_ast;
        }
        /* todo: As of now, there are no dot calls that yield an integer value.
           However, I suspect that when that occurs, dotcall will also need to
           be here. */
    }

    new_ast->parent = active;
}

/* merge_value
   This handles merging a var, call, or parenth expression. */
static void merge_value(lily_ast_pool *ap, lily_ast *new_ast)
{
    lily_ast *active = ap->active;

    if (active != NULL) {
        /* It's an oo call if we're merging a call against an existing
           value. */

        if (active->tree_type >= tree_typecast) {
            /* It's impossible to find another typecast here because inner
               typecasts are wrapped inside of a parenth tree. */
            if (active->right == NULL)
                active->right = new_ast;
            else if (active->right->tree_type == tree_unary)
                merge_unary(ap, active, new_ast);
            else
                merge_absorb(ap, active, new_ast);
        }
        else if (active->tree_type == tree_unary)
            merge_unary(ap, active, new_ast);
        else
            merge_absorb(ap, active, new_ast);
    }
    else {
        /* If no root, then no value or call so far. So become root, if only
           temporarily. */
        if (ap->root == NULL)
            ap->root = new_ast;

        ap->active = new_ast;
    }
}

/** Common merge helpers **/
/* make_new_tree
   Create a new tree, which becomes the start of the chain of available trees
   for the pool to use. The tree is returned as well, so it can be used.
   This raises ErrNoMem if unable to make a new tree. */
static lily_ast *make_new_tree(lily_ast_pool *ap)
{
    lily_ast *new_ast = lily_malloc(sizeof(lily_ast));
    if (new_ast == NULL)
        lily_raise_nomem(ap->raiser);

    new_ast->next_tree = ap->available_start;
    ap->available_start = new_ast;
    /* The pool will never re-use any trees that have already been used until
       the ast has been walked. Therefore, there is no need to update
       ->available_current. */

    return new_ast;
}

/* priority_for_op
   Returns the priority of a given op. The higher the number, the more important
   that it is. This usually follow's C's precedence table. */
static int priority_for_op(lily_expr_op o)
{
    int prio;

    switch (o) {
        case expr_assign:
        case expr_div_assign:
        case expr_mul_assign:
        case expr_plus_assign:
        case expr_minus_assign:
        case expr_left_shift_assign:
        case expr_right_shift_assign:
            prio = 0;
            break;
        case expr_logical_or:
            prio = 1;
            break;
        case expr_logical_and:
            prio = 2;
            break;
        case expr_eq_eq:
        case expr_not_eq:
            prio = 3;
            break;
        case expr_lt:
        case expr_gr:
        case expr_lt_eq:
        case expr_gr_eq:
            prio = 4;
            break;
        /* Bitwise ops are intentionally put before equality operations. This
           allows users to use bitwise ops without parens.
           This:        a & 0x10 == x
           Instead of: (a & 0x10) == x

           Keeping their different precendence levels for now though. */
        case expr_bitwise_or:
            prio = 5;
            break;
        case expr_bitwise_xor:
            prio = 6;
            break;
        case expr_bitwise_and:
            prio = 7;
            break;
        case expr_left_shift:
        case expr_right_shift:
            prio = 8;
            break;
        case expr_plus:
        case expr_minus:
            prio = 9;
            break;
        case expr_multiply:
        case expr_divide:
        case expr_modulo:
            prio = 10;
            break;
        case expr_unary_not:
        case expr_unary_minus:
            prio = 11;
            break;
        default:
            /* Won't happen, but makes -Wall happy. */
            prio = -1;
            break;
    }

    return prio;
}

/* push_tree_arg
   This takes the current root and adds it as an argument to the last tree that
   was entered. Call is the call, and tree is typically the root. */
static void push_tree_arg(lily_ast_pool *ap, lily_ast *call, lily_ast *tree)
{
    /* The args of a callable are linked to themselves, with the last one
       set to NULL. This will work for multiple functions, because the
       functions would be using different ASTs. */
    if (call->arg_start == NULL) {
        call->arg_start = tree;
        call->arg_top = tree;
    }
    else {
        call->arg_top->next_arg = tree;
        call->arg_top = tree;
    }

    /* Calls with 0 args have no value, so tree is null. */
    if (tree) {
        /* This ensures that subtrees know what they're contained in. This is
           essential for autocasts. */
        tree->parent = call;
        tree->next_arg = NULL;
        call->args_collected++;
    }
}

/** API for ast. The ast is responsible for creating and merging the trees in
    properly, instead of having the parser create the trees. Do note that trees
    are reused by setting the index to 0. Only relevant fields need to be set
    (ast will never check the arg_start of a unary op, for example). **/

/* lily_ast_collect_arg
   This function will take the pool's root and add it as an argument to the
   last tree that was entered and clear root+active for reuse. */
inline void lily_ast_collect_arg(lily_ast_pool *ap)
{
    /* This is where the function is. Don't drop the index, because it's not
       done yet. */
    lily_ast *a = ap->saved_trees[ap->save_index-1];

    push_tree_arg(ap, a, ap->root);

    /* Keep all of the expressions independent. */
    ap->root = NULL;
    ap->active = NULL;
}

/* lily_ast_enter_tree
   This begins an expression that takes comma-separated arguments. 'var' is only
   used by tree_call, and only when the call is a named variable. In all other
   cases, 'var' is NULL and ignored. */
void lily_ast_enter_tree(lily_ast_pool *ap, lily_tree_type tt, lily_var *var)
{
    lily_ast *a;
    if (ap->available_current) {
        a = ap->available_current;
        ap->available_current = a->next_tree;
    }
    else
        a = make_new_tree(ap);

    a->tree_type = tt;
    a->line_num = *ap->lex_linenum;
    a->result = (lily_sym *)var;
    a->args_collected = 0;
    a->arg_start = NULL;
    a->arg_top = NULL;

    merge_value(ap, a);

    /* This is used to save the current value. If this call will be current,
       then parent will be set to NULL properly later.
       The emitter checks this to see if the call returns to anything. */
    a->parent = ap->active;

    if (ap->save_index + 2 >= ap->save_size) {
        ap->save_size *= 2;
        lily_ast **new_saved;
        new_saved = lily_realloc(ap->saved_trees, sizeof(lily_ast *) *
                                 ap->save_size);

        if (new_saved == NULL)
            lily_raise_nomem(ap->raiser);

        ap->saved_trees = new_saved;
    }

    ap->saved_trees[ap->save_index] = ap->root;
    ap->saved_trees[ap->save_index+1] = a;
    ap->root = NULL;
    ap->active = NULL;

    ap->save_index += 2;
}

/* lily_ast_caller_tree_type
   This function returns the type of tree that is currently receiving arguments.
   This is used by parser to ensure that the proper token is used to close the
   tree. */
lily_tree_type lily_ast_caller_tree_type(lily_ast_pool *ap)
{
    lily_ast *a = ap->saved_trees[ap->save_index-1];

    return a->tree_type;
}

/* lily_ast_leave_tree
   This takes the pool's root and adds it as an argument to the last tree that
   was entered. Emitter will check the arg count when it does type checking. */
void lily_ast_leave_tree(lily_ast_pool *ap)
{
    ap->save_index--;
    lily_ast *a = ap->saved_trees[ap->save_index];

    push_tree_arg(ap, a, ap->root);

    ap->save_index--;
    ap->root = ap->saved_trees[ap->save_index];
    ap->active = a->parent;

    /* Current gets saved to a->parent when making a call. In some cases, the
       call was to be current, which makes the ast think it is the parent of
       itself. */
    if (a->parent == a)
        a->parent = NULL;
}

/* lily_ast_push_binary_op
   This 'creates' and merges a binary op against the active tree. */
void lily_ast_push_binary_op(lily_ast_pool *ap, lily_expr_op op)
{
    lily_ast *new_ast;
    if (ap->available_current) {
        new_ast = ap->available_current;
        ap->available_current = new_ast->next_tree;
    }
    else
        new_ast = make_new_tree(ap);

    lily_ast *active = ap->active;

    new_ast->tree_type = tree_binary;
    new_ast->line_num = *ap->lex_linenum;
    new_ast->priority = priority_for_op(op);
    new_ast->op = op;
    new_ast->left = NULL;
    new_ast->right = NULL;
    new_ast->parent = NULL;

    /* Active is always non-NULL, because binary always comes after a value of
       some kind. */
    if (active->tree_type < tree_binary) {
        /* Only a value or call so far. The binary op takes over. */
        if (ap->root == active)
            ap->root = new_ast;

        new_ast->left = active;
        ap->active = new_ast;
    }
    else if (active->tree_type == tree_binary) {
        /* Figure out how the two trees will fit together. */
        int new_prio, active_prio;
        new_prio = new_ast->priority;
        active_prio = active->priority;
        if ((new_prio > active_prio) || new_prio == 0) {
            /* The new tree goes before the current one, so it steals the rhs
               and becomes it (because lower trees have precedence). Since the
               new tree still needs a right, it becomes current.
               new_prio == 0 is so that assign and assign-like operations run
               right-to-left. */
            new_ast->left = active->right;
            active->right = new_ast;

            new_ast->parent = active;
            ap->active = new_ast;
        }
        else {
            /* This tree goes above the current one, and above any that have
               a priority <= to its own (<= and not < because the ops need
               to run left-to-right). Always active, and maybe root. */
            lily_ast *tree = active;
            while (tree->parent) {
                if (new_prio > tree->parent->priority)
                    break;

                tree = tree->parent;
            }
            if (tree->parent != NULL) {
                /* Think 'linked list insertion'. */
                lily_ast *parent = tree->parent;
                if (parent->left == tree)
                    parent->left = new_ast;
                else
                    parent->right = new_ast;

                new_ast->parent = active->parent;
            }
            else {
                /* Remember to operate on the tree, not current. */
                tree->parent = new_ast;
                ap->root = new_ast;
            }
            new_ast->left = tree;
            ap->active = new_ast;
        }
    }
}

/* lily_ast_push_empty_list
   This creates a tree_list tree with no inner values. ->sig is set so the list
   has a default sig. This is done because it's simpler than the enter/leave
   needed otherwise. It's easier to set the sig too. */
void lily_ast_push_empty_list(lily_ast_pool *ap, lily_sig *sig)
{
    lily_ast *a;
    if (ap->available_current) {
        a = ap->available_current;
        ap->available_current = a->next_tree;
    }
    else
        a = make_new_tree(ap);

    a->tree_type = tree_list;
    a->line_num = *ap->lex_linenum;
    a->result = NULL;
    a->args_collected = 0;
    a->arg_start = NULL;
    a->arg_top = NULL;
    a->sig = sig;

    merge_value(ap, a);
}

/* lily_ast_push_unary_op
   This 'creates' and merges a unary op against the active tree. */
void lily_ast_push_unary_op(lily_ast_pool *ap, lily_expr_op op)
{
    lily_ast *a;
    if (ap->available_current) {
        a = ap->available_current;
        ap->available_current = a->next_tree;
    }
    else
        a = make_new_tree(ap);

    lily_ast *active = ap->active;

    a->left = NULL;
    a->line_num = *ap->lex_linenum;
    a->tree_type = tree_unary;
    a->priority = priority_for_op(op);
    a->op = op;

    if (active != NULL) {
        if (active->tree_type == tree_var ||
            active->tree_type == tree_local_var ||
            active->tree_type == tree_call ||
            active->tree_type == tree_literal) {
            active->parent = a;
            ap->active = a;
            ap->root = a;
        }
        else
            merge_unary(ap, active, a);
    }
    else {
        ap->active = a;
        ap->root = a;
    }
}

void lily_ast_push_local_var(lily_ast_pool *ap, lily_var *var)
{
    lily_ast *a;
    if (ap->available_current) {
        a = ap->available_current;
        ap->available_current = a->next_tree;
    }
    else
        a = make_new_tree(ap);

    /* Local vars already have a register allocated. Mark them so that the
       emitter can do a no-op for them. */
    a->tree_type = tree_local_var;
    a->line_num = *ap->lex_linenum;
    a->result = (lily_sym *)var;

    merge_value(ap, a);
}

/* lily_ast_push_sym
   This creates and merges a symbol holding a value. This symbol can be either
   a literal, or a global var.
   These are separated from local vars because literals and globals need to be
   loaded into a register before use. */
void lily_ast_push_sym(lily_ast_pool *ap, lily_sym *s)
{
    lily_ast *a;
    if (ap->available_current) {
        a = ap->available_current;
        ap->available_current = a->next_tree;
    }
    else
        a = make_new_tree(ap);

    a->tree_type = tree_var;
    a->line_num = *ap->lex_linenum;
    a->result = s;

    merge_value(ap, a);
}

void lily_ast_push_literal(lily_ast_pool *ap, lily_literal *lit)
{
    lily_ast *a;
    if (ap->available_current) {
        a = ap->available_current;
        ap->available_current = a->next_tree;
    }
    else
        a = make_new_tree(ap);

    a->tree_type = tree_literal;
    a->line_num = *ap->lex_linenum;
    a->result = (lily_sym *)lit;

    merge_value(ap, a);
}

/* lily_ast_push_sig
   This 'creates' a typecast tree, and places a sig into it. The tree is merged
   against the active tree. Right is used to store the value because that allows
   typecast to share some code with binary trees in some areas. */
void lily_ast_push_sig(lily_ast_pool *ap, lily_sig *sig)
{
    lily_ast *a;
    if (ap->available_current) {
        a = ap->available_current;
        ap->available_current = a->next_tree;
    }
    else
        a = make_new_tree(ap);

    a->tree_type = tree_typecast;
    a->sig = sig;
    a->right = NULL;
    a->line_num = *ap->lex_linenum;
    a->result = NULL;

    merge_value(ap, a);
}
