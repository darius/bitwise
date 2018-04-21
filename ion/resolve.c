typedef enum SymKind {
    SYM_NONE,
    SYM_VAR,
    SYM_CONST,
    SYM_FUNC,
    SYM_TYPE,
} SymKind;

typedef enum SymState {
    SYM_UNRESOLVED,
    SYM_RESOLVING,
    SYM_RESOLVED,
} SymState;

typedef struct Sym {
    const char *name;
    SymKind kind;
    SymState state;
    Decl *decl;
    Type *type;
    Val val;
} Sym;

enum {
    MAX_LOCAL_SYMS = 1024
};

Decls *global_decls;
Sym **sorted_syms;
Map global_syms_map;
Sym **global_syms_buf;
Sym local_syms[MAX_LOCAL_SYMS];
Sym *local_syms_end = local_syms;

Sym *sym_new(SymKind kind, const char *name, Decl *decl) {
    Sym *sym = xcalloc(1, sizeof(Sym));
    sym->kind = kind;
    sym->name = name;
    sym->decl = decl;
    return sym;
}

Sym *sym_decl(Decl *decl) {
    SymKind kind = SYM_NONE;
    switch (decl->kind) {
    case DECL_STRUCT:
    case DECL_UNION:
    case DECL_TYPEDEF:
    case DECL_ENUM:
        kind = SYM_TYPE;
        break;
    case DECL_VAR:
        kind = SYM_VAR;
        break;
    case DECL_CONST:
        kind = SYM_CONST;
        break;
    case DECL_FUNC:
        kind = SYM_FUNC;
        break;
    default:
        assert(0);
        break;
    }
    Sym *sym = sym_new(kind, decl->name, decl);
    if (decl->kind == DECL_STRUCT || decl->kind == DECL_UNION) {
        sym->state = SYM_RESOLVED;
        sym->type = type_incomplete(sym);
    }
    return sym;
}

Sym *sym_get_local(const char *name) {
    for (Sym *it = local_syms_end; it != local_syms; it--) {
        Sym *sym = it-1;
        if (sym->name == name) {
            return sym;
        }
    }
    return NULL;
}

Sym *sym_get(const char *name) {
    Sym *sym = sym_get_local(name);
    return sym ? sym : map_get(&global_syms_map, (void *)name);
}

bool sym_push_var(const char *name, Type *type) {
    if (sym_get_local(name)) {
        return false;
    }
    if (local_syms_end == local_syms + MAX_LOCAL_SYMS) {
        fatal("Too many local symbols");
    }
    *local_syms_end++ = (Sym){
        .name = name,
        .kind = SYM_VAR,
        .state = SYM_RESOLVED,
        .type = type,
    };
    return true;
}

Sym *sym_enter(void) {
    return local_syms_end;
}

void sym_leave(Sym *sym) {
    local_syms_end = sym;
}

void sym_global_put(Sym *sym) {
    if (map_get(&global_syms_map, sym->name)) {
        SrcPos pos = sym->decl ? sym->decl->pos : pos_builtin;
        fatal_error(pos, "Duplicate definition of global symbol");
    }
    map_put(&global_syms_map, (void *)sym->name, sym);
    buf_push(global_syms_buf, sym);
}

void sym_global_type(const char *name, Type *type) {
    Sym *sym = sym_new(SYM_TYPE, str_intern(name), NULL);
    sym->state = SYM_RESOLVED;
    sym->type = type;
    sym_global_put(sym);
}

void sym_global_typedef(const char *name, Type *type) {
    Sym *sym = sym_new(SYM_TYPE, str_intern(name), new_decl_typedef(pos_builtin, name, new_typespec_name(pos_builtin, name)));
    sym->state = SYM_RESOLVED;
    sym->type = type;
    sym_global_put(sym);
}

void sym_global_const(const char *name, Type *type, Val val) {
    Sym *sym = sym_new(SYM_CONST, str_intern(name), NULL);
    sym->state = SYM_RESOLVED;
    sym->type = type;
    sym->val = val;
    sym_global_put(sym);
}

void sym_global_func(const char *name, Type *type) {
    assert(type->kind == TYPE_FUNC);
    Sym *sym = sym_new(SYM_FUNC, str_intern(name), NULL);
    sym->state = SYM_RESOLVED;
    sym->type = type;
    sym_global_put(sym);
}

Sym *sym_global_decl(Decl *decl) {
    Sym *sym = sym_decl(decl);
    sym_global_put(sym);
    if (decl->kind == DECL_ENUM) {
        sym->state = SYM_RESOLVED;
        sym->type = type_enum(sym);
        buf_push(sorted_syms, sym);
        for (int i = 0; i < decl->enum_decl.num_items; i++) {
            EnumItem item = decl->enum_decl.items[i];
            if (item.init) {
                fatal_error(item.pos, "Explicit enum constant initializers are not currently supported");
            }
            sym_global_const(item.name, sym->type, (Val){.i = i});
        }
    }
    return sym;
}

typedef struct Operand {
    Type *type;
    bool is_lvalue;
    bool is_const;
    Val val;
} Operand;

Operand operand_null;

Operand operand_rvalue(Type *type) {
    return (Operand){
        .type = unqualify_type(type),
    };
}

Operand operand_lvalue(Type *type) {
    return (Operand){
        .type = type,
        .is_lvalue = true,
    };
}

Operand operand_const(Type *type, Val val) {
    return (Operand){
        .type = unqualify_type(type),
        .is_const = true,
        .val = val,
    };
}

Operand operand_decay(Operand operand) {
    operand.type = unqualify_type(operand.type);
    if (operand.type->kind == TYPE_ARRAY) {
        operand.type = type_ptr(operand.type->base);
    }
    operand.is_lvalue = false;
    return operand;
}

bool is_null_ptr(Operand operand);

#define CASE(k, t) \
    case k: \
        switch (type->kind) { \
        case TYPE_BOOL: \
            operand->val.b = (bool)operand->val.t; \
            break; \
        case TYPE_CHAR: \
            operand->val.c = (char)operand->val.t; \
            break; \
        case TYPE_UCHAR: \
            operand->val.uc = (unsigned char)operand->val.t; \
            break; \
        case TYPE_SCHAR: \
            operand->val.sc = (signed char)operand->val.t; \
            break; \
        case TYPE_SHORT: \
            operand->val.s = (short)operand->val.t; \
            break; \
        case TYPE_USHORT: \
            operand->val.us = (unsigned short)operand->val.t; \
            break; \
        case TYPE_INT: \
        case TYPE_ENUM: \
            operand->val.i = (int)operand->val.t; \
            break; \
        case TYPE_UINT: \
            operand->val.u = (unsigned)operand->val.t; \
            break; \
        case TYPE_LONG: \
            operand->val.l = (long)operand->val.t; \
            break; \
        case TYPE_ULONG: \
            operand->val.ul = (unsigned long)operand->val.t; \
            break; \
        case TYPE_LLONG: \
            operand->val.ll = (long long)operand->val.t; \
            break; \
        case TYPE_ULLONG: \
            operand->val.ull = (unsigned long long)operand->val.t; \
            break; \
        case TYPE_PTR: \
            operand->val.p = (uintptr_t)operand->val.t; \
            break; \
        case TYPE_FLOAT: \
        case TYPE_DOUBLE: \
            break; \
        default: \
            operand->is_const = false; \
            break; \
        } \
        break;

bool is_convertible(Operand *operand, Type *dest) {
    dest = unqualify_type(dest);
    Type *src = unqualify_type(operand->type);
    if (dest == src) {
        return true;
    } else if (is_arithmetic_type(dest) && is_arithmetic_type(src)) {
        return true;
    } else if (is_ptr_type(dest) && is_null_ptr(*operand)) {
        return true;
    } else if (is_ptr_type(dest) && is_ptr_type(src)) {
        if (is_const_type(dest->base) && is_const_type(src->base)) {
            return dest->base->base == src->base->base || dest->base->base == type_void || src->base->base == type_void;
        } else {
            Type *unqual_dest_base = unqualify_type(dest->base);
            if (unqual_dest_base == src->base) {
                return true;
            } else if (unqual_dest_base == type_void) {
                return is_const_type(dest->base) || !is_const_type(src->base);
            } else {
                return src->base == type_void;
            }
        }
    } else {
        return false;
    }
}

bool is_castable(Operand *operand, Type *dest) {
    Type *src = operand->type;
    if (is_convertible(operand, dest)) {
        return true;
    } else if (is_integer_type(dest)) {
        return is_ptr_type(src);
    } else if (is_integer_type(src)) {
        return is_ptr_type(dest);
    } else if (is_ptr_type(dest) && is_ptr_type(src)) {
        return true;
    } else {
        return false;
    }
}

bool cast_operand(Operand *operand, Type *type) {
    Type *qual_type = type;
    type = unqualify_type(type);
    operand->type = unqualify_type(operand->type);
    if (operand->type != type) {
        if (!is_castable(operand, type)) {
            return false;
        }
        if (operand->is_const) {
            if (is_floating_type(operand->type)) {
                operand->is_const = !is_integer_type(type);
            } else {
                switch (operand->type->kind) {
                CASE(TYPE_BOOL, b)
                CASE(TYPE_CHAR, c)
                CASE(TYPE_UCHAR, uc)
                CASE(TYPE_SCHAR, sc)
                CASE(TYPE_SHORT, s)
                CASE(TYPE_USHORT, us)
                CASE(TYPE_INT, i)
                CASE(TYPE_ENUM, i)
                CASE(TYPE_UINT, u)
                CASE(TYPE_LONG, l)
                CASE(TYPE_ULONG, ul)
                CASE(TYPE_LLONG, ll)
                CASE(TYPE_ULLONG, ull)
                CASE(TYPE_PTR, p)
                default:
                    operand->is_const = false;
                    break;
                }
            }
        }
    }
    operand->type = qual_type;
    return true;
}

bool convert_operand(Operand *operand, Type *type) {
    if (is_convertible(operand, type)) {
        cast_operand(operand, type);
        *operand = operand_rvalue(operand->type);
        return true;
    }
    return false;
}

#undef CASE

bool is_null_ptr(Operand operand) {
    if (operand.is_const && (is_ptr_type(operand.type) || is_integer_type(operand.type))) {
        cast_operand(&operand, type_ullong);
        return operand.val.ull == 0;
    } else {
        return false;
    }
}

void promote_operand(Operand *operand) {
    switch (operand->type->kind) {
    case TYPE_BOOL:
    case TYPE_CHAR:
    case TYPE_SCHAR:
    case TYPE_UCHAR:
    case TYPE_SHORT:
    case TYPE_USHORT:
    case TYPE_ENUM:
        cast_operand(operand, type_int);
        break;
    default:
        // Do nothing
        break;
    }
}

void unify_arithmetic_operands(Operand *left, Operand *right) {
    if (left->type == type_double) {
        cast_operand(right, type_double);
    } else if (right->type == type_double) {
        cast_operand(left, type_double);
    } else if (left->type == type_float) {
        cast_operand(right, type_float);
    } else if (right->type == type_float) {
        cast_operand(left, type_float);
    } else {
        assert(is_integer_type(left->type));
        assert(is_integer_type(right->type));
        promote_operand(left);
        promote_operand(right);
        if (left->type != right->type) {
            if (is_signed_type(left->type) == is_signed_type(right->type)) {
                if (type_rank(left->type) <= type_rank(right->type)) {
                    cast_operand(left, right->type);
                } else {
                    cast_operand(right, left->type);
                }
            } else if (is_signed_type(left->type) && type_rank(right->type) >= type_rank(left->type)) {
                cast_operand(left, right->type);
            } else if (is_signed_type(right->type) && type_rank(left->type) >= type_rank(right->type)) {
                cast_operand(right, left->type);
            } else if (is_signed_type(left->type) && type_sizeof(left->type) > type_sizeof(right->type)) {
                cast_operand(right, left->type);            
            } else if (is_signed_type(right->type) && type_sizeof(right->type) > type_sizeof(left->type)) {
                cast_operand(left, right->type);
            } else { 
                Type *type = unsigned_type(is_signed_type(left->type) ? left->type : right->type);
                cast_operand(left, type);
                cast_operand(right, type);
            }
        }
    }
    assert(left->type == right->type);
}

Map resolved_type_map;

Type *get_resolved_type(void *ptr) {
    return map_get(&resolved_type_map, ptr);
}

void set_resolved_type(void *ptr, Type *type) {
    map_put(&resolved_type_map, ptr, type);
}

Sym *resolve_name(const char *name);
Operand resolve_const_expr(Expr *expr);
Operand resolve_expected_expr(Expr *expr, Type *expected_type);

Operand resolve_expr(Expr *expr) {
    return resolve_expected_expr(expr, NULL);
}

Operand resolve_expr_rvalue(Expr *expr) {
    return operand_decay(resolve_expr(expr));
}

Operand resolve_expected_expr_rvalue(Expr *expr, Type *expected_type) {
    return operand_decay(resolve_expected_expr(expr, expected_type));
}

Type *resolve_typespec(Typespec *typespec) {
    if (!typespec) {
        return type_void;
    }
    Type *result = NULL;
    switch (typespec->kind) {
    case TYPESPEC_NAME: {
        Sym *sym = resolve_name(typespec->name);
        if (!sym) {
            fatal_error(typespec->pos, "Unresolved type name");
        }
        if (sym->kind != SYM_TYPE) {
            fatal_error(typespec->pos, "%s must denote a type", typespec->name);
            return NULL;
        }
        result = sym->type;
        break;
    }
    case TYPESPEC_CONST:
        result = type_const(resolve_typespec(typespec->base));
        break;
    case TYPESPEC_PTR:
        result = type_ptr(resolve_typespec(typespec->base));
        break;
    case TYPESPEC_ARRAY: {
        int size = 0;
        if (typespec->num_elems) {
            Operand operand = resolve_const_expr(typespec->num_elems);
            if (!is_integer_type(operand.type)) {
                fatal_error(typespec->pos, "Array size constant expression must have integer type");
            }
            cast_operand(&operand, type_int);
            size = operand.val.i;
            if (size <= 0) {
                fatal_error(typespec->num_elems->pos, "Non-positive array size");
            }
        }
        result = type_array(resolve_typespec(typespec->base), size);
        break;
    }
    case TYPESPEC_FUNC: {
        Type **args = NULL;
        for (size_t i = 0; i < typespec->func.num_args; i++) {
            Type *arg = resolve_typespec(typespec->func.args[i]);
            if (arg == type_void) {
                fatal_error(typespec->pos, "Function parameter type cannot be void");
            }
            buf_push(args, arg);
        }
        Type *ret = type_void;
        if (typespec->func.ret) {
            ret = resolve_typespec(typespec->func.ret);
        }
        if (is_array_type(ret)) {
            fatal_error(typespec->pos, "Function return type cannot be array");
        }
        result = type_func(args, buf_len(args), ret, false);
        break;
    }
    default:
        assert(0);
        return NULL;
    }
    set_resolved_type(typespec, result);
    return result;
}

void complete_type(Type *type) {
    if (type->kind == TYPE_COMPLETING) {
        fatal_error(type->sym->decl->pos, "Type completion cycle");
        return;
    } else if (type->kind != TYPE_INCOMPLETE) {
        return;
    }
    Decl *decl = type->sym->decl;
    if (decl->is_incomplete) {
        fatal_error(decl->pos, "Trying to use incomplete type as complete type");
    }
    type->kind = TYPE_COMPLETING;
    assert(decl->kind == DECL_STRUCT || decl->kind == DECL_UNION);
    TypeField *fields = NULL;
    for (size_t i = 0; i < decl->aggregate.num_items; i++) {
        AggregateItem item = decl->aggregate.items[i];
        Type *item_type = resolve_typespec(item.type);
        complete_type(item_type);
        if (type_sizeof(item_type) == 0) {
            fatal_error(item.pos, "Field type of size 0 is not allowed");
        }
        for (size_t j = 0; j < item.num_names; j++) {
            buf_push(fields, (TypeField){item.names[j], item_type});
        }
    }
    if (buf_len(fields) == 0) {
        fatal_error(decl->pos, "No fields");
    }
    if (has_duplicate_fields(fields, buf_len(fields))) {
        fatal_error(decl->pos, "Duplicate fields");
    }
    if (decl->kind == DECL_STRUCT) {
        type_complete_struct(type, fields, buf_len(fields));
    } else {
        assert(decl->kind == DECL_UNION);
        type_complete_union(type, fields, buf_len(fields));
    }
    buf_push(sorted_syms, type->sym);
}

Type *resolve_decl_type(Decl *decl) {
    assert(decl->kind == DECL_TYPEDEF);
    return resolve_typespec(decl->typedef_decl.type);
}

Type *resolve_decl_var(Decl *decl) {
    assert(decl->kind == DECL_VAR);
    Type *type = NULL;
    if (decl->var.type) {
        type = resolve_typespec(decl->var.type);
    }
    if (decl->var.expr) {
        Operand operand = resolve_expected_expr(decl->var.expr, type);
        if (type) {
            if (is_incomplete_array_type(type) && is_array_type(operand.type)) {
                // Incomplete array size, so infer the size from the initializer expression's type.
            } else {
                if (!convert_operand(&operand, type)) {
                    fatal_error(decl->pos, "Invalid type in variable initializer");
                }
            }
        }
        type = operand.type;
    }
    complete_type(type);
    if (type->size == 0) {
        fatal_error(decl->pos, "Cannot declare variable of size 0");
    }
    return type;
}

Type *resolve_decl_const(Decl *decl, Val *val) {
    assert(decl->kind == DECL_CONST);
    Operand result = resolve_const_expr(decl->const_decl.expr);
    if (!is_scalar_type(result.type)) {
        fatal_error(decl->pos, "Const declarations must have scalar type");
    }
    if (decl->const_decl.type) {
        Type *type = resolve_typespec(decl->const_decl.type);
        if (!convert_operand(&result, type)) {
            fatal_error(decl->pos, "Invalid type in constant declaration");
        }
    }
    *val = result.val;
    return result.type;
}

Type *resolve_decl_func(Decl *decl) {
    assert(decl->kind == DECL_FUNC);
    Type **params = NULL;
    for (size_t i = 0; i < decl->func.num_params; i++) {
        Type *param = resolve_typespec(decl->func.params[i].type);
        complete_type(param);
        if (param == type_void) {
            fatal_error(decl->pos, "Function parameter type cannot be void");
        }
        buf_push(params, param);
    }
    Type *ret_type = type_void;
    if (decl->func.ret_type) {
        ret_type = resolve_typespec(decl->func.ret_type);
        complete_type(ret_type);
    }
    if (is_array_type(ret_type)) {
        fatal_error(decl->pos, "Function return type cannot be array");
    }
    return type_func(params, buf_len(params), ret_type, decl->func.has_varargs);
}

typedef struct StmtCtx {
    bool is_break_legal;
    bool is_continue_legal;
} StmtCtx;

bool resolve_stmt(Stmt *stmt, Type *ret_type, StmtCtx ctx);

void resolve_cond_expr(Expr *expr) {
    Operand cond = resolve_expr_rvalue(expr);
    if (!is_scalar_type(cond.type)) {
        fatal_error(expr->pos, "Conditional expression must have operand type");
    }
}

bool resolve_stmt_block(StmtList block, Type *ret_type, StmtCtx ctx) {
    Sym *scope = sym_enter();
    bool returns = false;
    for (size_t i = 0; i < block.num_stmts; i++) {
        returns = resolve_stmt(block.stmts[i], ret_type, ctx) || returns;
    }
    sym_leave(scope);
    return returns;
}

Operand resolve_expr_binary_op(TokenKind op, const char *op_name, SrcPos pos, Operand left, Operand right);

void resolve_stmt_assign(Stmt *stmt) {
    assert(stmt->kind == STMT_ASSIGN);
    Operand left = resolve_expr(stmt->assign.left);
    if (!left.is_lvalue) {
        fatal_error(stmt->pos, "Cannot assign to non-lvalue");
    }
    if (is_array_type(left.type)) {
        fatal_error(stmt->pos, "Cannot assign to array");
    }
    if (left.type->nonmodifiable) {
        fatal_error(stmt->pos, "Left-hand side of assignment has non-modifiable type");
    }
    const char *assign_op_name = token_kind_name(stmt->assign.op);
    TokenKind binary_op = assign_token_to_binary_token[stmt->assign.op];
    Operand right = resolve_expected_expr_rvalue(stmt->assign.right, left.type);
    Operand result;
    if (stmt->assign.op == TOKEN_ASSIGN) {
        result = right;
    } else if (stmt->assign.op == TOKEN_ADD_ASSIGN || stmt->assign.op == TOKEN_SUB_ASSIGN) {
        if (left.type->kind == TYPE_PTR && is_integer_type(right.type)) {
            result = operand_rvalue(left.type);
        } else if (is_arithmetic_type(left.type) && is_arithmetic_type(right.type)) {
            result = resolve_expr_binary_op(binary_op, assign_op_name, stmt->pos, left, right);
        } else {
            fatal_error(stmt->pos, "Invalid operand types for %s", assign_op_name);
        }
    } else {
        result = resolve_expr_binary_op(binary_op, assign_op_name, stmt->pos, left, right);
    }
    if (!convert_operand(&result, left.type)) {
        fatal_error(stmt->pos, "Invalid type in assignment");
    }
}

void resolve_stmt_init(Stmt *stmt) {
    assert(stmt->kind == STMT_INIT);
    Type *type;
    if (stmt->init.type) {
        type = resolve_typespec(stmt->init.type);
        if (stmt->init.expr) {
            Type *expected_type = unqualify_type(type);
            Operand operand = resolve_expected_expr(stmt->init.expr, expected_type);
            if (is_incomplete_array_type(type) && is_array_type(operand.type) && type->base == operand.type->base) {
                type = operand.type;
            } else if (!convert_operand(&operand, expected_type)) {
                fatal_error(stmt->pos, "Invalid type in initialization statement");
            }
        }
    } else {
        assert(stmt->init.expr);
        type = unqualify_type(resolve_expr(stmt->init.expr).type);
    }
    if (type->size == 0) {
        fatal_error(stmt->pos, "Cannot declare variable of size 0");
    }
    if (!sym_push_var(stmt->init.name, type)) {
        fatal_error(stmt->pos, "Shadowed definition of local symbol");
    }
}

bool resolve_stmt(Stmt *stmt, Type *ret_type, StmtCtx ctx) {
    switch (stmt->kind) {
    case STMT_RETURN:
        if (stmt->expr) {
            Operand operand = resolve_expected_expr(stmt->expr, ret_type);
            if (!convert_operand(&operand, ret_type)) {
                fatal_error(stmt->pos, "Invalid type in return expression");
            }
        } else if (ret_type != type_void) {
            fatal_error(stmt->pos, "Empty return expression for function with non-void return type");
        }
        return true;
    case STMT_BREAK:
        if (!ctx.is_break_legal) {
            fatal_error(stmt->pos, "Illegal break");
        }
        return false;
    case STMT_CONTINUE:
        if (!ctx.is_continue_legal) {
            fatal_error(stmt->pos, "Illegal continue");
        }
        return false;
    case STMT_BLOCK:
        return resolve_stmt_block(stmt->block, ret_type, ctx);
    case STMT_NOTE:
        if (stmt->note.name == assert_name) {
            if (stmt->note.num_args != 1) {
                fatal_error(stmt->pos, "#assert takes 1 argument");
            }
            resolve_cond_expr(stmt->note.args[0].expr);
        } else {
            warning(stmt->pos, "Unknown statement #directive '%s'", stmt->note.name);
        }
        return false;
    case STMT_IF: {
        resolve_cond_expr(stmt->if_stmt.cond);
        bool returns = resolve_stmt_block(stmt->if_stmt.then_block, ret_type, ctx);
        for (size_t i = 0; i < stmt->if_stmt.num_elseifs; i++) {
            ElseIf elseif = stmt->if_stmt.elseifs[i];
            resolve_cond_expr(elseif.cond);
            returns = resolve_stmt_block(elseif.block, ret_type, ctx) && returns;
        }
        if (stmt->if_stmt.else_block.stmts) {
            returns = resolve_stmt_block(stmt->if_stmt.else_block, ret_type, ctx) && returns;
        } else {
            returns = false;
        }
        return returns;
    }
    case STMT_WHILE:
    case STMT_DO_WHILE:
        resolve_cond_expr(stmt->while_stmt.cond);
        ctx.is_break_legal = true;
        ctx.is_continue_legal = true;
        resolve_stmt_block(stmt->while_stmt.block, ret_type, ctx);
        return false;
    case STMT_FOR: {
        Sym *scope = sym_enter();
        if (stmt->for_stmt.init) {
            resolve_stmt(stmt->for_stmt.init, ret_type, ctx);
        }
        if (stmt->for_stmt.cond) {
            resolve_cond_expr(stmt->for_stmt.cond);
        }
        if (stmt->for_stmt.next) {
            resolve_stmt(stmt->for_stmt.next, ret_type, ctx);
        }
        ctx.is_break_legal = true;
        ctx.is_continue_legal = true;
        resolve_stmt_block(stmt->for_stmt.block, ret_type, ctx);
        sym_leave(scope);
        return false;
    }
    case STMT_SWITCH: {
        Operand operand = resolve_expr_rvalue(stmt->switch_stmt.expr);
        if (!is_integer_type(operand.type)) {
            fatal_error(stmt->pos, "Switch expression must have integer type");
        }
        ctx.is_break_legal = true;
        bool returns = true;
        bool has_default = false;
        for (size_t i = 0; i < stmt->switch_stmt.num_cases; i++) {
            SwitchCase switch_case = stmt->switch_stmt.cases[i];
            for (size_t j = 0; j < switch_case.num_exprs; j++) {
                Expr *case_expr = switch_case.exprs[j];
                Operand case_operand = resolve_expr(case_expr);
                if (!convert_operand(&case_operand, operand.type)) {
                    fatal_error(case_expr->pos, "Invalid type in switch case expression");
                }
            }
            if (switch_case.is_default) {
                if (has_default) {
                    fatal_error(stmt->pos, "Switch statement has multiple default clauses");
                }
                has_default = true;
            }
            if (switch_case.block.num_stmts > 0) {
                Stmt *last_stmt = switch_case.block.stmts[switch_case.block.num_stmts - 1];
                if (last_stmt->kind == STMT_BREAK) {
                    warning(last_stmt->pos, "Case blocks already end with an implicit break");
                }
            }
            returns = resolve_stmt_block(switch_case.block, ret_type, ctx) && returns;
        }
        return returns && has_default;
    }
    case STMT_ASSIGN:
        resolve_stmt_assign(stmt);
        return false;
    case STMT_INIT:
        resolve_stmt_init(stmt);
        return false;
    case STMT_EXPR:
        resolve_expr(stmt->expr);
        return false;
    default:
        assert(0);
        return false;
    }
}

void resolve_func_body(Sym *sym) {
    Decl *decl = sym->decl;
    assert(decl->kind == DECL_FUNC);
    assert(sym->state == SYM_RESOLVED);
    if (decl->is_incomplete) {
        return;
    }
    Sym *scope = sym_enter();
    for (size_t i = 0; i < decl->func.num_params; i++) {
        FuncParam param = decl->func.params[i];
        Type *param_type = resolve_typespec(param.type);
        if (is_array_type(param_type)) {
            param_type = type_ptr(param_type->base);
        }
        sym_push_var(param.name, param_type);
    }
    Type *ret_type = resolve_typespec(decl->func.ret_type);
    assert(!is_array_type(ret_type));
    bool returns = resolve_stmt_block(decl->func.block, ret_type, (StmtCtx){0});
    sym_leave(scope);
    if (ret_type != type_void && !returns) {
        fatal_error(decl->pos, "Not all control paths return values");
    }
}

void resolve_sym(Sym *sym) {
    if (sym->state == SYM_RESOLVED) {
        return;
    } else if (sym->state == SYM_RESOLVING) {
        fatal_error(sym->decl->pos, "Cyclic dependency");
        return;
    }
    assert(sym->state == SYM_UNRESOLVED);
    sym->state = SYM_RESOLVING;
    switch (sym->kind) {
    case SYM_TYPE:
        sym->type = resolve_decl_type(sym->decl);
        break;
    case SYM_VAR:
        sym->type = resolve_decl_var(sym->decl);
        break;
    case SYM_CONST:
        sym->type = resolve_decl_const(sym->decl, &sym->val);
        break;
    case SYM_FUNC:
        sym->type = resolve_decl_func(sym->decl);
        break;
    default:
        assert(0);
        break;
    }
    sym->state = SYM_RESOLVED;
    buf_push(sorted_syms, sym);
}

void finalize_sym(Sym *sym) {
    resolve_sym(sym);
    if (sym->kind == SYM_TYPE) {
        if (sym->decl && sym->decl->is_incomplete) {
            return;
        }
        complete_type(sym->type);
    } else if (sym->kind == SYM_FUNC) {
        resolve_func_body(sym);
    }
}

Sym *resolve_name(const char *name) {
    Sym *sym = sym_get(name);
    if (!sym) {
        return NULL;
    }
    resolve_sym(sym);
    return sym;
}

Operand resolve_expr_field(Expr *expr) {
    assert(expr->kind == EXPR_FIELD);
    Operand operand = resolve_expr(expr->field.expr);
    bool was_const_type = is_const_type(operand.type);
    Type *type = unqualify_type(operand.type);
    complete_type(type);
    if (is_ptr_type(type)) {
        operand = operand_lvalue(type->base);
        was_const_type = is_const_type(operand.type);
        type = unqualify_type(operand.type);
        complete_type(type);
    }
    if (type->kind != TYPE_STRUCT && type->kind != TYPE_UNION) {
        fatal_error(expr->pos, "Can only access fields on aggregates or pointers to aggregates");
        return operand_null;
    }
    for (size_t i = 0; i < type->aggregate.num_fields; i++) {
        TypeField field = type->aggregate.fields[i];
        if (field.name == expr->field.name) {
            Operand field_operand = operand.is_lvalue ? operand_lvalue(field.type) : operand_rvalue(field.type);
            if (was_const_type) {
                field_operand.type = type_const(field_operand.type);
            }
            return field_operand;
        }
    }
    fatal_error(expr->pos, "No field named '%s'", expr->field.name);
    return operand_null;
}

long long eval_unary_op_ll(TokenKind op, long long val) {
    switch (op) {
    case TOKEN_ADD:
        return +val;
    case TOKEN_SUB:
        return -val;
    case TOKEN_NEG:
        return ~val;
    case TOKEN_NOT:
        return !val;
    default:
        assert(0);
        break;
    }
    return 0;
}

unsigned long long eval_unary_op_ull(TokenKind op, unsigned long long val) {
    switch (op) {
    case TOKEN_ADD:
        return +val;
    case TOKEN_SUB:
        return 0ull - val;
    case TOKEN_NEG:
        return ~val;
    case TOKEN_NOT:
        return !val;
    default:
        assert(0);
        break;
    }
    return 0;
}

long long eval_binary_op_ll(TokenKind op, long long left, long long right) {
    switch (op) {
    case TOKEN_MUL:
        return left * right;
    case TOKEN_DIV:
        return right != 0 ? left / right : 0;
    case TOKEN_MOD:
        return right != 0 ? left % right : 0;
    case TOKEN_AND:
        return left & right;
    case TOKEN_LSHIFT:
        return left << right;
    case TOKEN_RSHIFT:
        return left >> right;
    case TOKEN_ADD:
        return left + right;
    case TOKEN_SUB:
        return left - right;
    case TOKEN_OR:
        return left | right;
    case TOKEN_XOR:
        return left ^ right;
    case TOKEN_EQ:
        return left == right;
    case TOKEN_NOTEQ:
        return left != right;
    case TOKEN_LT:
        return left < right;
    case TOKEN_LTEQ:
        return left <= right;
    case TOKEN_GT:
        return left > right;
    case TOKEN_GTEQ:
        return left >= right;
    default:
        assert(0);
        break;
    }
    return 0;
}

unsigned long long eval_binary_op_ull(TokenKind op, unsigned long long left, unsigned long long right) {
    switch (op) {
    case TOKEN_MUL:
        return left * right;
    case TOKEN_DIV:
        return right != 0 ? left / right : 0;
    case TOKEN_MOD:
        return right != 0 ? left % right : 0;
    case TOKEN_AND:
        return left & right;
    case TOKEN_LSHIFT:
        return left << right;
    case TOKEN_RSHIFT:
        return left >> right;
    case TOKEN_ADD:
        return left + right;
    case TOKEN_SUB:
        return left - right;
    case TOKEN_OR:
        return left | right;
    case TOKEN_XOR:
        return left ^ right;
    case TOKEN_EQ:
        return left == right;
    case TOKEN_NOTEQ:
        return left != right;
    case TOKEN_LT:
        return left < right;
    case TOKEN_LTEQ:
        return left <= right;
    case TOKEN_GT:
        return left > right;
    case TOKEN_GTEQ:
        return left >= right;
    default:
        assert(0);
        break;
    }
    return 0;
}

Val eval_unary_op(TokenKind op, Type *type, Val val) {
    if (is_integer_type(type)) {
        Operand operand = operand_const(type, val);
        if (is_signed_type(type)) {
            cast_operand(&operand, type_llong);
            operand.val.ll = eval_unary_op_ll(op, operand.val.ll);
        } else {
            cast_operand(&operand, type_ullong);
            operand.val.ll = eval_unary_op_ull(op, operand.val.ull);
        }
        cast_operand(&operand, type);
        return operand.val;
    } else {
        return (Val){0};
    }
}

Val eval_binary_op(TokenKind op, Type *type, Val left, Val right) {
    if (is_integer_type(type)) {
        Operand left_operand = operand_const(type, left);
        Operand right_operand = operand_const(type, right);
        Operand result_operand;
        if (is_signed_type(type)) {
            cast_operand(&left_operand, type_llong);
            cast_operand(&right_operand, type_llong);
            result_operand = operand_const(type_llong, (Val){.ll = eval_binary_op_ll(op, left_operand.val.ll, right_operand.val.ll)});
        } else {
            cast_operand(&left_operand, type_ullong);
            cast_operand(&right_operand, type_ullong);
            result_operand = operand_const(type_ullong, (Val){.ull = eval_binary_op_ull(op, left_operand.val.ull, right_operand.val.ull)});
        }
        cast_operand(&result_operand, type);
        return result_operand.val;
    } else {
        return (Val){0};
    }
}

Operand resolve_expr_name(Expr *expr) {
    assert(expr->kind == EXPR_NAME);
    Sym *sym = resolve_name(expr->name);
    if (!sym) {
        fatal_error(expr->pos, "Unresolved name '%s'", expr->name);
    }
    if (sym->kind == SYM_VAR) {
        return operand_lvalue(sym->type);
    } else if (sym->kind == SYM_CONST) {
        return operand_const(sym->type, sym->val);
    } else if (sym->kind == SYM_FUNC) {
        return operand_rvalue(sym->type);
    } else {
        fatal_error(expr->pos, "%s must be a var or const", expr->name);
        return operand_null;
    }
}

Operand resolve_unary_op(TokenKind op, Operand operand) {
    promote_operand(&operand);
    if (operand.is_const) {
        return operand_const(operand.type, eval_unary_op(op, operand.type, operand.val));
    } else {
        return operand;
    }
}

Operand resolve_expr_unary(Expr *expr) {
    assert(expr->kind == EXPR_UNARY);
    if (expr->unary.op == TOKEN_AND) {
        Operand operand = resolve_expr(expr->unary.expr);
        if (!operand.is_lvalue) {
            fatal_error(expr->pos, "Cannot take address of non-lvalue");
        }
        return operand_rvalue(type_ptr(operand.type));
    } else {
        Operand operand = resolve_expr_rvalue(expr->unary.expr);
        Type *type = operand.type;
        switch (expr->unary.op) {
        case TOKEN_MUL:
            if (!is_ptr_type(type)) {
                fatal_error(expr->pos, "Cannot deref non-ptr type");
            }
            return operand_lvalue(type->base);
        case TOKEN_ADD:
        case TOKEN_SUB:
            if (!is_arithmetic_type(type)) {
                fatal_error(expr->pos, "Can only use unary %s with arithmetic types", token_kind_name(expr->unary.op));
            }
            return resolve_unary_op(expr->unary.op, operand);
        case TOKEN_NEG:
            if (!is_integer_type(type)) {
                fatal_error(expr->pos, "Can only use ~ with integer types");
            }
            return resolve_unary_op(expr->unary.op, operand);
        case TOKEN_NOT:
            if (!is_scalar_type(type)) {
                fatal_error(expr->pos," Can only use ! with scalar types");
            }
            return resolve_unary_op(expr->unary.op, operand);
        default:
            assert(0);
            break;
        }
        return (Operand){0};
    }
}

Operand resolve_binary_op(TokenKind op, Operand left, Operand right) {
    if (left.is_const && right.is_const) {
        return operand_const(left.type, eval_binary_op(op, left.type, left.val, right.val));
    } else {
        return operand_rvalue(left.type);
    }
}

Operand resolve_binary_arithmetic_op(TokenKind op, Operand left, Operand right) {
    unify_arithmetic_operands(&left, &right);
    return resolve_binary_op(op, left, right);
}

Operand resolve_expr_binary_op(TokenKind op, const char *op_name, SrcPos pos, Operand left, Operand right) {
    switch (op) {
    case TOKEN_MUL:
    case TOKEN_DIV:
        if (!is_arithmetic_type(left.type)) {
            fatal_error(pos, "Left operand of %s must have arithmetic type", op_name);
        }
        if (!is_arithmetic_type(right.type)) {
            fatal_error(pos, "Right operand of %s must have arithmetic type", op_name);
        }
        return resolve_binary_arithmetic_op(op, left, right);
    case TOKEN_MOD:
        if (!is_integer_type(left.type)) {
            fatal_error(pos, "Left operand of %% must have integer type");
        }
        if (!is_integer_type(right.type)) {
            fatal_error(pos, "Right operand of %% must have integer type");
        }
        return resolve_binary_arithmetic_op(op, left, right);
    case TOKEN_ADD:
        if (is_arithmetic_type(left.type) && is_arithmetic_type(right.type)) {
            return resolve_binary_arithmetic_op(op, left, right);
        } else if (is_ptr_type(left.type) && is_integer_type(right.type)) {
            return operand_rvalue(left.type);
        } else if (is_ptr_type(right.type) && is_integer_type(left.type)) {
            return operand_rvalue(right.type);
        } else {
            fatal_error(pos, "Operands of + must both have arithmetic type, or pointer and integer type");
        }
        break;
    case TOKEN_SUB:
        if (is_arithmetic_type(left.type) && is_arithmetic_type(right.type)) {
            return resolve_binary_arithmetic_op(op, left, right);
        } else if (is_ptr_type(left.type) && is_integer_type(right.type)) {
            return operand_rvalue(left.type);
        } else if (is_ptr_type(left.type) && is_ptr_type(right.type)) {
            if (left.type->base != right.type->base) {
                fatal_error(pos, "Cannot subtract pointers to different types");
            }
            return operand_rvalue(type_ssize);
        } else {
            fatal_error(pos, "Operands of - must both have arithmetic type, pointer and integer type, or compatible pointer types");
        }
        break;
    case TOKEN_LSHIFT:
    case TOKEN_RSHIFT:
        if (is_integer_type(left.type) && is_integer_type(right.type)) {
            promote_operand(&left);
            promote_operand(&right);
            Type *result_type = left.type;
            Operand result;
            if (is_signed_type(left.type)) {
                cast_operand(&left, type_llong);
                cast_operand(&right, type_llong);
            } else {
                cast_operand(&left, type_ullong);
                cast_operand(&right, type_ullong);
            }
            result = resolve_binary_op(op, left, right);
            cast_operand(&result, result_type);
            return result;
        } else {
            fatal_error(pos, "Operands of %s must both have integer type", op_name);
        }
        break;
    case TOKEN_LT:
    case TOKEN_LTEQ:
    case TOKEN_GT:
    case TOKEN_GTEQ:
    case TOKEN_EQ:
    case TOKEN_NOTEQ:
        if (is_arithmetic_type(left.type) && is_arithmetic_type(right.type)) {
            Operand result = resolve_binary_arithmetic_op(op, left, right);
            cast_operand(&result, type_int);
            return result;
        } else if (is_ptr_type(left.type) && is_ptr_type(right.type)) {
            if (left.type->base != right.type->base) {
                fatal_error(pos, "Cannot compare pointers to different types");
            }
            return operand_rvalue(type_int);
        } else if ((is_null_ptr(left) && is_ptr_type(right.type)) || (is_null_ptr(right) && is_ptr_type(left.type))) {
            return operand_rvalue(type_int);
        } else {
            fatal_error(pos, "Operands of %s must be arithmetic types or compatible pointer types", op_name);
        }
        break;
    case TOKEN_AND:
    case TOKEN_XOR:
    case TOKEN_OR:
        if (is_integer_type(left.type) && is_integer_type(right.type)) {
            return resolve_binary_arithmetic_op(op, left, right);
        } else {
            fatal_error(pos, "Operands of %s must have arithmetic types", op_name);
        }
        break;
    case TOKEN_AND_AND:
    case TOKEN_OR_OR:
        if (is_scalar_type(left.type) && is_scalar_type(right.type)) {
            if (left.is_const && right.is_const) {
                cast_operand(&left, type_bool);
                cast_operand(&right, type_bool);
                int i;
                if (op == TOKEN_AND_AND) {
                    i = left.val.b && right.val.b;
                } else {
                    assert(op == TOKEN_OR_OR);
                    i = left.val.b || right.val.b;
                }
                return operand_const(type_int, (Val){.i = i});
            } else {
                return operand_rvalue(type_int);
            }
        } else {
            fatal_error(pos, "Operands of %s must have scalar types", op_name);
        }
        break;
    default:
        assert(0);
        break;
    }
    return (Operand){0};
}

Operand resolve_expr_binary(Expr *expr) {
    assert(expr->kind == EXPR_BINARY);
    Operand left = resolve_expr_rvalue(expr->binary.left);
    Operand right = resolve_expr_rvalue(expr->binary.right);
    TokenKind op = expr->binary.op;
    const char *op_name = token_kind_name(op);
    return resolve_expr_binary_op(op, op_name, expr->pos, left, right);
}

int aggregate_field_index(Type *type, const char *name) {
    assert(type->kind == TYPE_STRUCT || type->kind == TYPE_UNION);
    for (int i = 0; i < type->aggregate.num_fields; i++) {
        if (type->aggregate.fields[i].name == name) {
            return i;
        }
    }
    return -1;
}

Operand resolve_expr_compound(Expr *expr, Type *expected_type) {
    assert(expr->kind == EXPR_COMPOUND);
    if (!expected_type && !expr->compound.type) {
        fatal_error(expr->pos, "Implicitly typed compound literals used in context without expected type");
    }
    Type *type = NULL;
    if (expr->compound.type) {
        type = resolve_typespec(expr->compound.type);
    } else {
        type = expected_type;
    }
    complete_type(type);
    bool is_const = is_const_type(type);
    type = unqualify_type(type);
    if (type->kind == TYPE_STRUCT || type->kind == TYPE_UNION) {
        int index = 0;
        for (size_t i = 0; i < expr->compound.num_fields; i++) {
            CompoundField field = expr->compound.fields[i];
            if (field.kind == FIELD_INDEX) {
                fatal_error(field.pos, "Index field initializer not allowed for struct/union compound literal");
            } else if (field.kind == FIELD_NAME) {
                index = aggregate_field_index(type, field.name);
                if (index == -1) {
                    fatal_error(field.pos, "Named field in compound literal does not exist");
                }
            }
            if (index >= type->aggregate.num_fields) {
                fatal_error(field.pos, "Field initializer in struct/union compound literal out of range");
            }
            Type *field_type = type->aggregate.fields[index].type;
            Operand init = resolve_expected_expr_rvalue(field.init, field_type);
            if (!convert_operand(&init, field_type)) {
                fatal_error(field.pos, "Invalid type in compound literal initializer");
            }
            index++;
        }
    } else if (type->kind == TYPE_ARRAY) {
        int index = 0, max_index = 0;
        for (size_t i = 0; i < expr->compound.num_fields; i++) {
            CompoundField field = expr->compound.fields[i];
            if (field.kind == FIELD_NAME) {
                fatal_error(field.pos, "Named field initializer not allowed for array compound literals");
            } else if (field.kind == FIELD_INDEX) {
                Operand operand = resolve_const_expr(field.index);
                if (!is_integer_type(operand.type)) {
                    fatal_error(field.pos, "Field initializer index expression must have type int");
                }
                if (!cast_operand(&operand, type_int)) {
                    fatal_error(field.pos, "Invalid type in field initializer index");
                }
                if (operand.val.i < 0) {
                    fatal_error(field.pos, "Field initializer index cannot be negative");
                }
                index = operand.val.i;
            }
            if (type->num_elems && index >= type->num_elems) {
                fatal_error(field.pos, "Field initializer in array compound literal out of range");
            }
            Operand init = resolve_expected_expr_rvalue(field.init, type->base);
            if (!convert_operand(&init, type->base)) {
                fatal_error(field.pos, "Invalid type in compound literal initializer");
            }
            max_index = MAX(max_index, index);
            index++;
        }
        if (type->num_elems == 0) {
            type = type_array(type->base, max_index + 1);
        }
    } else {
        assert(is_scalar_type(type));
        if (expr->compound.num_fields > 1) {
            fatal_error(expr->pos, "Compound literal for scalar type cannot have more than one operand");
        }
        if (expr->compound.num_fields == 1) {
            CompoundField field = expr->compound.fields[0];
            Operand init = resolve_expected_expr_rvalue(field.init, type);
            if (!convert_operand(&init, type)) {
                fatal_error(field.pos, "Invalid type in compound literal initializer");
            }
        }
    }
    return operand_lvalue(is_const ? type_const(type) : type);
}

Operand resolve_expr_call(Expr *expr) {
    assert(expr->kind == EXPR_CALL);
    if (expr->call.expr->kind == EXPR_NAME) {
        Sym *sym = resolve_name(expr->call.expr->name);
        if (sym && sym->kind == SYM_TYPE) {
            if (expr->call.num_args != 1) {
                fatal_error(expr->pos, "Type conversion operator takes 1 argument");
            }
            Operand operand = resolve_expr_rvalue(expr->call.args[0]);
            if (!cast_operand(&operand, sym->type)) {
                fatal_error(expr->pos, "Invalid type cast");
            }
            return operand;
        }
    }
    Operand func = resolve_expr_rvalue(expr->call.expr);
    if (func.type->kind != TYPE_FUNC) {
        fatal_error(expr->pos, "Cannot call non-function value");
    }
    size_t num_params = func.type->func.num_params;
    if (expr->call.num_args < num_params) {
        fatal_error(expr->pos, "Function call with too few arguments");
    }
    if (expr->call.num_args > num_params && !func.type->func.has_varargs) {
        fatal_error(expr->pos, "Function call with too many arguments");
    }
    for (size_t i = 0; i < num_params; i++) {
        Type *param_type = func.type->func.params[i];
        Operand arg = resolve_expected_expr_rvalue(expr->call.args[i], param_type);
        if (is_array_type(param_type)) {
            param_type = type_ptr(param_type->base);
        }
        if (!convert_operand(&arg, param_type)) {
            fatal_error(expr->call.args[i]->pos, "Invalid type in function call argument");
        }
    }
    for (size_t i = num_params; i < expr->call.num_args; i++) {
        resolve_expr_rvalue(expr->call.args[i]);
    }
    return operand_rvalue(func.type->func.ret);
}

Operand resolve_expr_ternary(Expr *expr, Type *expected_type) {
    assert(expr->kind == EXPR_TERNARY);
    Operand cond = resolve_expr_rvalue(expr->ternary.cond);
    if (!is_scalar_type(cond.type)) {
        fatal_error(expr->pos, "Ternary conditional must have scalar type");
    }
    Operand left = resolve_expected_expr_rvalue(expr->ternary.then_expr, expected_type);
    Operand right = resolve_expected_expr_rvalue(expr->ternary.else_expr, expected_type);
    if (is_arithmetic_type(left.type) && is_arithmetic_type(right.type)) {
        unify_arithmetic_operands(&left, &right);
        if (cond.is_const && left.is_const && right.is_const) {
            return operand_const(left.type, cond.val.i ? left.val : right.val);
        } else {
            return operand_rvalue(left.type);
        }
    } else if (left.type == right.type) {
        return operand_rvalue(left.type);
    } else {
        fatal_error(expr->pos, "Left and right operands of ternary expression must have arithmetic types or identical types");
    }
}

Operand resolve_expr_index(Expr *expr) {
    assert(expr->kind == EXPR_INDEX);
    Operand operand = resolve_expr_rvalue(expr->index.expr);
    if (!is_ptr_type(operand.type)) {
        fatal_error(expr->pos, "Can only index arrays and pointers");
    }
    Operand index = resolve_expr_rvalue(expr->index.index);
    if (!is_integer_type(index.type)) {
        fatal_error(expr->pos, "Index must have integer type");
    }
    return operand_lvalue(operand.type->base);
}

Operand resolve_expr_cast(Expr *expr) {
    assert(expr->kind == EXPR_CAST);
    Type *type = resolve_typespec(expr->cast.type);
    Operand operand = resolve_expr_rvalue(expr->cast.expr);
    if (!cast_operand(&operand, type)) {
        fatal_error(expr->pos, "Invalid type cast");
    }
    return operand;
}

Operand resolve_expr_int(Expr *expr) {
    assert(expr->kind == EXPR_INT);
    unsigned long long val = expr->int_lit.val;
    Operand operand = operand_const(type_ullong, (Val){.ull = val});
    Type *type = type_ullong;
    if (expr->int_lit.mod == MOD_NONE) {
        bool overflow = false;  
        switch (expr->int_lit.suffix) {
        case SUFFIX_NONE:
            type = type_int;
            // TODO: MAX constants should be sourced from the backend target table, not from the host compiler's header files.
            if (val > INT_MAX) {
                type = type_long;
                if (val > LONG_MAX) {
                    type = type_llong;
                    overflow = val > LLONG_MAX;
                }
            }
            break;
        case SUFFIX_U:
            type = type_uint;
            if (val > UINT_MAX) {
                type = type_ulong;
                if (val > ULONG_MAX) {
                    type = type_ullong;
                }
            }
            break;
        case SUFFIX_L:
            type = type_long;
            if (val > LONG_MAX) {
                type = type_llong;
                overflow = val > LLONG_MAX;
            }
            break;
        case SUFFIX_UL:
            type = type_ulong;
            if (val > ULONG_MAX) {
                type = type_ullong;
            }
            break;
        case SUFFIX_LL:
            type = type_llong;
            overflow = val > LLONG_MAX;
            break;
        case SUFFIX_ULL:
            type = type_ullong;
            break;
        default:
            assert(0);
            break;
        }
        if (overflow) {
            fatal_error(expr->pos, "Integer literal overflow");
        }
    } else {
        switch (expr->int_lit.suffix) {
        case SUFFIX_NONE:
            type = type_int;
            if (val > INT_MAX) {
                type = type_uint;
                if (val > UINT_MAX) {
                    type = type_long;
                    if (val > LONG_MAX) {
                        type = type_ulong;
                        if (val > ULONG_MAX) {
                            type = type_llong;
                            if (val > LLONG_MAX) {
                                type = type_ullong;
                            }
                        }
                    }
                }
            }
            break;
        case SUFFIX_U:
            type = type_uint;
            if (val > UINT_MAX) {
                type = type_ulong;
                if (val > ULONG_MAX) {
                    type = type_ullong;
                }
            }
            break;
        case SUFFIX_L:
            type = type_long;
            if (val > LONG_MAX) {
                type = type_ulong;
                if (val > ULONG_MAX) {
                    type = type_llong;
                    if (val > LLONG_MAX) {
                        type = type_ullong;
                    }
                }
            }
            break;
        case SUFFIX_UL:
            type = type_ulong;
            if (val > ULONG_MAX) {
                type = type_ullong;
            }
            break;
        case SUFFIX_LL:
            type = type_llong;
            if (val > LLONG_MAX) {
                type = type_ullong;
            }
            break;
        case SUFFIX_ULL:
            type = type_ullong;
            break;
        default:
            assert(0);
            break;
        }
    }
    cast_operand(&operand, type);
    return operand;
}

Operand resolve_expr_modify(Expr *expr) {
    Operand operand = resolve_expr(expr->modify.expr);
    Type *type = operand.type;
    complete_type(type);
    if (!operand.is_lvalue) {
        fatal_error(expr->pos, "Cannot modify non-lvalue");
    }
    if (type->nonmodifiable) {
        fatal_error(expr->pos, "Cannot modify non-modifiable type");
    }
    if (!(is_integer_type(type) || type->kind == TYPE_PTR)) {
        fatal_error(expr->pos, "%s only valid for integer and pointer types", token_kind_name(expr->modify.op));
    }
    return operand_rvalue(type);
}

Operand resolve_expected_expr(Expr *expr, Type *expected_type) {
    Operand result;
    switch (expr->kind) {
    case EXPR_INT:
        result = resolve_expr_int(expr);
        break;  
    case EXPR_FLOAT:
        result = operand_const(expr->float_lit.suffix == SUFFIX_D ? type_double : type_float, (Val){0});
        break;
    case EXPR_STR:
        result = operand_rvalue(type_ptr(type_char));
        break;
    case EXPR_NAME:
        result = resolve_expr_name(expr);
        break;
    case EXPR_CAST:
        result = resolve_expr_cast(expr);
        break;
    case EXPR_CALL:
        result = resolve_expr_call(expr);
        break;
    case EXPR_INDEX:
        result = resolve_expr_index(expr);
        break;
    case EXPR_FIELD:
        result = resolve_expr_field(expr);
        break;
    case EXPR_COMPOUND:
        result = resolve_expr_compound(expr, expected_type);
        break;
    case EXPR_UNARY:
        result = resolve_expr_unary(expr);
        break;
    case EXPR_BINARY:
        result = resolve_expr_binary(expr);
        break;
    case EXPR_TERNARY:
        result = resolve_expr_ternary(expr, expected_type);
        break;
    case EXPR_SIZEOF_EXPR: {
        if (expr->sizeof_expr->kind == EXPR_NAME) {
            Sym *sym = resolve_name(expr->sizeof_expr->name);
            if (sym && sym->kind == SYM_TYPE) {
                complete_type(sym->type);
                result = operand_const(type_usize, (Val){.ull = type_sizeof(sym->type)});
                set_resolved_type(expr->sizeof_expr, sym->type);
                break;
            }
        }
        Type *type = resolve_expr(expr->sizeof_expr).type;
        complete_type(type);
        result = operand_const(type_usize, (Val){.ull = type_sizeof(type)});
        break;
    }
    case EXPR_SIZEOF_TYPE: {
        Type *type = resolve_typespec(expr->sizeof_type);
        complete_type(type);
        result = operand_const(type_usize, (Val){.ull = type_sizeof(type)});
        break;
    }
    case EXPR_ALIGNOF_EXPR: {
        if (expr->sizeof_expr->kind == EXPR_NAME) {
            Sym *sym = resolve_name(expr->alignof_expr->name);
            if (sym && sym->kind == SYM_TYPE) {
                complete_type(sym->type);
                result = operand_const(type_usize, (Val){.ull = type_alignof(sym->type)});
                set_resolved_type(expr->alignof_expr, sym->type);
                break;
            }
        }
        Type *type = resolve_expr(expr->alignof_expr).type;
        complete_type(type);
        result = operand_const(type_usize, (Val){.ull = type_alignof(type)});
        break;
    }
    case EXPR_ALIGNOF_TYPE: {
        Type *type = resolve_typespec(expr->alignof_type);
        complete_type(type);
        result = operand_const(type_usize, (Val){.ull = type_alignof(type)});
        break;
    }
    case EXPR_TYPEOF_TYPE: {
        Type *type = resolve_typespec(expr->typeof_type);
        result = operand_const(type_int, (Val){.i = type->typeid});
        break;
    }
    case EXPR_TYPEOF_EXPR: {
        if (expr->typeof_expr->kind == EXPR_NAME) {
            Sym *sym = resolve_name(expr->typeof_expr->name);
            if (sym && sym->kind == SYM_TYPE) {
                result = operand_const(type_int, (Val){.i = sym->type->typeid});
                set_resolved_type(expr->typeof_expr, sym->type);
                break;
            }
        }
        Type *type = resolve_expr(expr->typeof_expr).type;
        result = operand_const(type_int, (Val){.i = type->typeid});
        break;
    }
    case EXPR_OFFSETOF: {
        Type *type = resolve_typespec(expr->offsetof_field.type);
        if (type->kind != TYPE_STRUCT && type->kind != TYPE_UNION) {
            fatal_error(expr->pos, "offsetof can only be used with struct/union types");
        }
        int field = aggregate_field_index(type, expr->offsetof_field.name);
        if (field < 0) {
            fatal_error(expr->pos, "No field '%s' in type", expr->offsetof_field.name);
        }
        result = operand_const(type_usize, (Val){.ull = type->aggregate.fields[field].offset});
        break;
    }
    case EXPR_MODIFY:
        result = resolve_expr_modify(expr);
        break;
    default:
        assert(0);
        result = operand_null;
        break;
    }
    set_resolved_type(expr, result.type);
    return result;
}

Operand resolve_const_expr(Expr *expr) {
    Operand result = resolve_expr(expr);
    if (!result.is_const) {
        fatal_error(expr->pos, "Expected constant expression");
    }
    return result;
}

Map decl_note_names;

void init_builtins(void) {
    static bool is_init;
    if (is_init) {
        return;
    }

    map_put(&decl_note_names, declare_note_name, (void *)1);

    sym_global_type("void", type_void);
    sym_global_type("bool", type_bool);
    sym_global_type("char", type_char);
    sym_global_type("schar", type_schar);
    sym_global_type("uchar", type_uchar);
    sym_global_type("short", type_short);
    sym_global_type("ushort", type_ushort);
    sym_global_type("int", type_int);
    sym_global_type("uint", type_uint);
    sym_global_type("long", type_long);
    sym_global_type("ulong", type_ulong);
    sym_global_type("llong", type_llong);
    sym_global_type("ullong", type_ullong);
    sym_global_type("float", type_float);
    sym_global_type("double", type_double);

    sym_global_typedef("uint8", type_uchar);
    sym_global_typedef("int8", type_schar);
    sym_global_typedef("uint16", type_ushort);
    sym_global_typedef("int16", type_short);
    sym_global_typedef("uint32", type_uint);
    sym_global_typedef("int32", type_int);
    sym_global_typedef("uint64", type_ullong);
    sym_global_typedef("int64", type_llong);

    sym_global_typedef("usize", type_usize);
    sym_global_typedef("ssize", type_ssize);
    sym_global_typedef("uintptr", type_uintptr);
    sym_global_typedef("typeid", type_int);

    sym_global_const("true", type_bool, (Val){.b = true});
    sym_global_const("false", type_bool, (Val){.b = false});
    sym_global_const("NULL", type_ptr(type_void), (Val){.p = 0});

    is_init = true;
}

void sym_global_decls(void) {
    for (size_t i = 0; i < global_decls->num_decls; i++) {
        Decl *decl = global_decls->decls[i];
        if (decl->kind == DECL_NOTE) {
            if (!map_get(&decl_note_names, decl->note.name)) {
                warning(decl->pos, "Unknown declaration #directive '%s'", decl->note.name);
            }
            if (decl->note.name == declare_note_name) {
                if (decl->note.num_args != 1) {
                    fatal_error(decl->pos, "#declare_note takes 1 argument");
                }
                Expr *arg = decl->note.args[0].expr;
                if (arg->kind != EXPR_NAME) {
                    fatal_error(decl->pos, "#declare_note argument must be name");
                }
                map_put(&decl_note_names, arg->name, (void *)1);
            }
        } else {
            sym_global_decl(global_decls->decls[i]);
        }
    }
}

void finalize_syms(void) {
    for (Sym **it = global_syms_buf; it != buf_end(global_syms_buf); it++) {
        Sym *sym = *it;
        if (sym->decl) {
            finalize_sym(sym);
        }
    }
}
