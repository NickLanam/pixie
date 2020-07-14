#include "src/carnot/planner/compiler/ast_visitor.h"

#include "src/carnot/planner/compiler_error_context/compiler_error_context.h"
#include "src/carnot/planner/ir/pattern_match.h"
#include "src/carnot/planner/objects/collection_object.h"
#include "src/carnot/planner/objects/expr_object.h"
#include "src/carnot/planner/objects/module.h"
#include "src/carnot/planner/objects/none_object.h"
#include "src/carnot/planner/objects/pixie_module.h"
#include "src/carnot/planner/objects/type_object.h"
#include "src/carnot/planner/parser/parser.h"

namespace pl {
namespace carnot {
namespace planner {
namespace compiler {
using pl::shared::scriptspb::VisFuncsInfo;
using pypa::AstType;

StatusOr<FuncIR::Op> ASTVisitorImpl::GetOp(const std::string& python_op, const pypa::AstPtr node) {
  auto op_find = FuncIR::op_map.find(python_op);
  if (op_find == FuncIR::op_map.end()) {
    return CreateAstError(node, "Operator '$0' not handled", python_op);
  }
  return op_find->second;
}

StatusOr<FuncIR::Op> ASTVisitorImpl::GetUnaryOp(const std::string& python_op,
                                                const pypa::AstPtr node) {
  auto op_find = FuncIR::unary_op_map.find(python_op);
  if (op_find == FuncIR::op_map.end()) {
    return CreateAstError(node, "Unary Operator '$0' not handled", python_op);
  }
  return op_find->second;
}

StatusOr<std::shared_ptr<ASTVisitorImpl>> ASTVisitorImpl::Create(
    IR* graph, CompilerState* compiler_state, ModuleHandler* module_handler, bool func_based_exec,
    const absl::flat_hash_set<std::string>& reserved_names,
    const absl::flat_hash_map<std::string, std::string>& module_map) {
  std::shared_ptr<ASTVisitorImpl> ast_visitor = std::shared_ptr<ASTVisitorImpl>(new ASTVisitorImpl(
      graph, compiler_state, VarTable::Create(), func_based_exec, reserved_names, module_handler));

  PL_RETURN_IF_ERROR(ast_visitor->InitGlobals());
  PL_RETURN_IF_ERROR(ast_visitor->SetupModules(module_map));
  return ast_visitor;
}

std::shared_ptr<ASTVisitorImpl> ASTVisitorImpl::CreateChild() {
  return CreateChildImpl(var_table_->CreateChild());
}

std::shared_ptr<ASTVisitor> ASTVisitorImpl::CreateModuleVisitor(
    std::shared_ptr<VarTable> var_table) {
  return std::static_pointer_cast<ASTVisitor>(CreateChildImpl(var_table));
}

std::shared_ptr<ASTVisitorImpl> ASTVisitorImpl::CreateChildImpl(
    std::shared_ptr<VarTable> var_table) {
  // The flag values should come from the parent var table, not be copied here.
  auto visitor = std::shared_ptr<ASTVisitorImpl>(new ASTVisitorImpl(
      ir_graph_, compiler_state_, var_table, func_based_exec_, {}, module_handler_));
  return visitor;
}

Status ASTVisitorImpl::SetupModules(
    const absl::flat_hash_map<std::string, std::string>& module_name_to_pxl_map) {
  DCHECK(module_handler_);
  PL_ASSIGN_OR_RETURN(
      (*module_handler_)[PixieModule::kPixieModuleObjName],
      PixieModule::Create(ir_graph_, compiler_state_, this, func_based_exec_, reserved_names_));
  for (const auto& [module_name, module_text] : module_name_to_pxl_map) {
    PL_ASSIGN_OR_RETURN((*module_handler_)[module_name], Module::Create(module_text, this));
  }
  return Status::OK();
}

Status ASTVisitorImpl::InitGlobals() {
  // Populate the type objects
  PL_ASSIGN_OR_RETURN(auto string_type_object, TypeObject::Create(IRNodeType::kString, this));
  var_table_->Add(ASTVisitorImpl::kStringTypeName, string_type_object);
  PL_ASSIGN_OR_RETURN(auto int_type_object, TypeObject::Create(IRNodeType::kInt, this));
  var_table_->Add(ASTVisitorImpl::kIntTypeName, int_type_object);
  PL_ASSIGN_OR_RETURN(auto float_type_object, TypeObject::Create(IRNodeType::kFloat, this));
  var_table_->Add(ASTVisitorImpl::kFloatTypeName, float_type_object);
  PL_ASSIGN_OR_RETURN(auto bool_type_object, TypeObject::Create(IRNodeType::kBool, this));
  var_table_->Add(ASTVisitorImpl::kBoolTypeName, bool_type_object);
  // Populate other reserved words
  var_table_->Add(ASTVisitorImpl::kNoneName, std::make_shared<NoneObject>(this));

  return CreateBoolLiterals();
}

Status ASTVisitorImpl::CreateBoolLiterals() {
  auto bool_ast = std::make_shared<pypa::Ast>(pypa::AstType::Bool);
  bool_ast->line = 0;
  bool_ast->column = 0;
  PL_ASSIGN_OR_RETURN(auto true_ir, ir_graph_->CreateNode<BoolIR>(bool_ast, true));
  PL_ASSIGN_OR_RETURN(auto true_object, ExprObject::Create(true_ir, this));
  var_table_->Add(ASTVisitorImpl::kTrueName, true_object);
  PL_ASSIGN_OR_RETURN(auto false_ir, ir_graph_->CreateNode<BoolIR>(bool_ast, false));
  PL_ASSIGN_OR_RETURN(auto false_object, ExprObject::Create(false_ir, this));
  var_table_->Add(ASTVisitorImpl::kFalseName, false_object);
  return Status::OK();
}

Status ASTVisitorImpl::ProcessExprStmtNode(const pypa::AstExpressionStatementPtr& e) {
  OperatorContext op_context({}, "", {});
  return Process(e->expr, op_context).status();
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessSingleExpressionModule(
    const pypa::AstModulePtr& module) {
  OperatorContext op_context({}, "");
  const std::vector<pypa::AstStmt>& items_list = module->body->items;
  if (items_list.size() != 1) {
    return CreateAstError(module,
                          "ProcessModuleExpression only works for single lined statements.");
  }
  const pypa::AstStmt& stmt = items_list[0];
  switch (stmt->type) {
    case pypa::AstType::ExpressionStatement: {
      return Process(PYPA_PTR_CAST(ExpressionStatement, stmt)->expr, op_context);
    }
    default: {
      return CreateAstError(module, "Want expression, got $0", GetAstTypeName(stmt->type));
    }
  }
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ParseAndProcessSingleExpression(
    std::string_view single_expr_str, bool import_px) {
  Parser parser;
  PL_ASSIGN_OR_RETURN(pypa::AstModulePtr ast,
                      parser.Parse(single_expr_str.data(), /* parse_doc_strings */ false));
  if (import_px) {
    auto child_visitor = CreateChild();
    // Use a child of this ASTVisitor so that we can add px to its child var_table without affecting
    // top-level visitor state.
    child_visitor->var_table_->Add(
        PixieModule::kPixieModuleObjName,
        (*child_visitor->module_handler_)[PixieModule::kPixieModuleObjName]);
    return child_visitor->ProcessSingleExpressionModule(ast);
  }

  return ProcessSingleExpressionModule(ast);
}

Status ASTVisitorImpl::ProcessModuleNode(const pypa::AstModulePtr& m) {
  PL_RETURN_IF_ERROR(ProcessASTSuite(m->body, /*is_function_definition_body*/ false));
  return Status::OK();
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ParseStringAsType(const pypa::AstPtr& ast,
                                                        const std::string& value,
                                                        std::shared_ptr<TypeObject> type) {
  ExpressionIR* node;
  switch (type->data_type()) {
    case types::DataType::BOOLEAN: {
      bool val;
      if (!absl::SimpleAtob(value, &val)) {
        return CreateAstError(ast, "Failed to parse arg with value '$0' as bool.", value);
      }
      PL_ASSIGN_OR_RETURN(node, ir_graph()->CreateNode<BoolIR>(ast, val));
      break;
    }
    case types::DataType::STRING: {
      PL_ASSIGN_OR_RETURN(node, ir_graph()->CreateNode<StringIR>(ast, value));
      break;
    }
    case types::DataType::INT64: {
      int64_t val;
      if (!absl::SimpleAtoi(value, &val)) {
        return CreateAstError(ast, "Failed to parse arg with value '$0' as int64.", value);
      }
      PL_ASSIGN_OR_RETURN(node, ir_graph()->CreateNode<IntIR>(ast, val));
      break;
    }
    case types::DataType::FLOAT64: {
      double val;
      if (!absl::SimpleAtod(value, &val)) {
        return CreateAstError(ast, "Failed to parse arg with value '$0' as float64.", value);
      }
      PL_ASSIGN_OR_RETURN(node, ir_graph()->CreateNode<FloatIR>(ast, val));
      break;
    }
    case types::DataType::DURATION64NS:
    case types::DataType::TIME64NS: {
      int64_t val;
      if (!absl::SimpleAtoi(value, &val)) {
        return CreateAstError(ast, "Failed to parse arg with value '$0' as time.", value);
      }
      PL_ASSIGN_OR_RETURN(node, ir_graph()->CreateNode<TimeIR>(ast, val));
      break;
    }
    case types::DataType::UINT128: {
      // TODO(james): Implement this.
      return CreateAstError(ast, "Passing arg of type UINT128 is currently unsupported.");
      break;
    }
    default:
      return CreateAstError(
          ast, "All arguments to executed functions must have an underlying concrete type.");
  }
  return ExprObject::Create(node, this);
}

StatusOr<ArgMap> ASTVisitorImpl::ProcessExecFuncArgs(const pypa::AstPtr& ast,
                                                     const std::shared_ptr<FuncObject>& func,
                                                     const ArgValues& arg_values) {
  ArgMap args;
  for (const auto arg : arg_values) {
    if (std::find(func->arguments().begin(), func->arguments().end(), arg.name()) ==
        func->arguments().end()) {
      return CreateAstError(ast, "Function '$0' does not have an argument called '$1'",
                            func->name(), arg.name());
    }
    auto it = func->arg_types().find(arg.name());
    if (it == func->arg_types().end()) {
      return CreateAstError(ast, "Arg type annotation required. Function: '$0', arg: '$1'",
                            func->name(), arg.name());
    }
    // TODO(james): Parse the string as an expression instead of adhoc parsing.
    PL_ASSIGN_OR_RETURN(auto node, ParseStringAsType(ast, arg.value(), it->second));
    // FuncObject::Call has logic to handle accepting normal args as kwargs,
    // so its easiest to just pass everything as kwargs. In the future, if we want to support
    // variadic args in exec funcs we will have to change this.
    args.kwargs.emplace_back(arg.name(), node);
  }
  return args;
}

Status ASTVisitorImpl::ProcessExecFuncs(const ExecFuncs& exec_funcs) {
  // TODO(James): handle errors here better. For now I have a fake ast ptr with -1 for line and col.
  auto ast = std::make_shared<pypa::AstExpressionStatement>();
  ast->line = 0;
  ast->column = 0;
  for (const auto& func : exec_funcs) {
    if (func.func_name() == "") {
      return CreateAstError(ast, "Must specify func_name for each FuncToExecute.");
    }
    if (func.output_table_prefix() == "") {
      return CreateAstError(ast, "Output_table_prefix must be specified for function $0.",
                            func.func_name());
    }

    // Get Function Object.
    auto objptr = var_table_->Lookup(func.func_name());
    if (objptr == nullptr) {
      return CreateAstError(ast, "Function to execute, '$0', not found.", func.func_name());
    }
    if (objptr->type() != QLObjectType::kFunction) {
      return CreateAstError(ast, "'$0' is a '$1' not a function.", func.func_name(),
                            objptr->name());
    }
    auto func_obj = std::static_pointer_cast<FuncObject>(objptr);

    // Process arguments.
    ArgValues arg_values(func.arg_values().begin(), func.arg_values().end());
    PL_ASSIGN_OR_RETURN(auto argmap, ProcessExecFuncArgs(ast, func_obj, arg_values));

    // Call function.
    PL_ASSIGN_OR_RETURN(auto return_obj, func_obj->Call(argmap, ast));

    // Process returns.
    if (!CollectionObject::IsCollection(return_obj)) {
      if (return_obj->type() != QLObjectType::kDataframe) {
        return CreateAstError(ast, "Function '$0' returns '$1' but should return a DataFrame.",
                              func.func_name(), return_obj->name());
      }
      auto df = std::static_pointer_cast<Dataframe>(return_obj);
      std::vector<std::string> out_columns;
      PL_RETURN_IF_ERROR(ir_graph()->CreateNode<MemorySinkIR>(
          ast, df->op(), func.output_table_prefix(), out_columns));
      continue;
    }

    auto return_collection = std::static_pointer_cast<CollectionObject>(return_obj);
    for (const auto& [i, obj] : Enumerate(return_collection->items())) {
      if (obj->type() != QLObjectType::kDataframe) {
        return CreateAstError(
            ast, "Function '$0' returns '$1' at index $2. All returned objects must be dataframes.",
            func.func_name(), obj->name(), i);
      }
      auto df = std::static_pointer_cast<Dataframe>(obj);
      auto out_name = absl::Substitute("$0[$1]", func.output_table_prefix(), i);
      std::vector<std::string> columns;
      PL_RETURN_IF_ERROR(ir_graph()->CreateNode<MemorySinkIR>(ast, df->op(), out_name, columns));
    }
  }
  return Status::OK();
}

StatusOr<shared::scriptspb::FuncArgsSpec> ASTVisitorImpl::GetMainFuncArgsSpec() const {
  QLObjectPtr mainfn = var_table_->Lookup(kMainFuncId);
  if (mainfn == nullptr) {
    return error::InvalidArgument("Could not find '$0' fn", kMainFuncId);
  }

  if (mainfn->type() != QLObjectType::kFunction) {
    return error::InvalidArgument("'$0' is not a function", kMainFuncId);
  }

  auto func_object = std::static_pointer_cast<FuncObject>(mainfn);
  return func_object->CreateFuncArgsSpec();
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessASTSuite(const pypa::AstSuitePtr& body,
                                                      bool is_function_definition_body) {
  pypa::AstStmtList items_list = body->items;
  if (items_list.size() == 0) {
    return CreateAstError(body, "No runnable code found");
  }
  if (items_list[0]->type == pypa::AstType::DocString) {
    if (!is_function_definition_body) {
      PL_ASSIGN_OR_RETURN(auto doc_string,
                          ProcessDocString(PYPA_PTR_CAST(DocString, items_list[0])));
      var_table_->Add("__doc__", doc_string);
    }
    // NOTE(james): If this is a function definition body we can still erase, since we handle
    // function docstrings at function definition time.
    items_list.erase(items_list.begin());
  } else {
    if (!is_function_definition_body) {
      PL_ASSIGN_OR_RETURN(auto ir_node, ir_graph_->CreateNode<StringIR>(body, ""));
      PL_ASSIGN_OR_RETURN(auto doc_string, ExprObject::Create(ir_node, this));
      var_table_->Add("__doc__", doc_string);
    }
  }
  // iterate through all the items on this list.
  for (pypa::AstStmt stmt : items_list) {
    switch (stmt->type) {
      case pypa::AstType::Import: {
        PL_RETURN_IF_ERROR(ProcessImport(PYPA_PTR_CAST(Import, stmt)));
        break;
      }
      case pypa::AstType::ImportFrom: {
        PL_RETURN_IF_ERROR(ProcessImportFrom(PYPA_PTR_CAST(ImportFrom, stmt)));
        break;
      }
      case pypa::AstType::ExpressionStatement: {
        PL_RETURN_IF_ERROR(ProcessExprStmtNode(PYPA_PTR_CAST(ExpressionStatement, stmt)));
        break;
      }
      case pypa::AstType::Assign: {
        PL_RETURN_IF_ERROR(ProcessAssignNode(PYPA_PTR_CAST(Assign, stmt)));
        break;
      }
      case pypa::AstType::FunctionDef: {
        PL_RETURN_IF_ERROR(ProcessFunctionDefNode(PYPA_PTR_CAST(FunctionDef, stmt)));
        break;
      }
      case pypa::AstType::DocString: {
        return CreateAstError(stmt,
                              "Doc strings are only allowed at the start of a module or function.");
      }
      case pypa::AstType::Return: {
        // If we are not parsing a function definition's body then we must error.
        if (!is_function_definition_body) {
          return CreateAstError(stmt, "'return' outside function");
        }
        // We exit early if the function definition return is used.
        return ProcessFuncDefReturn(PYPA_PTR_CAST(Return, stmt));
      }
      default: {
        return CreateAstError(stmt, "Can't parse expression of type $0",
                              GetAstTypeName(stmt->type));
      }
    }
  }
  // If we reach the end of the stmt list before hitting a return, return a NoneObject.
  return std::static_pointer_cast<QLObject>(std::make_shared<NoneObject>(body, this));
}

Status ASTVisitorImpl::AddPixieModule(std::string_view as_name) {
  PL_ASSIGN_OR_RETURN(auto px, PixieModule::Create(ir_graph_, compiler_state_, this,
                                                   func_based_exec_, reserved_names_));
  // TODO(philkuz) verify this is done before hand in a parent var table if one exists.
  var_table_->Add(as_name, px);
  return Status::OK();
}

std::string AsNameFromAlias(const pypa::AstAliasPtr& alias) {
  std::string name = PYPA_PTR_CAST(Name, alias->name)->id;

  if (alias->as_name == nullptr) {
    return name;
  }
  return PYPA_PTR_CAST(Name, alias->as_name)->id;
}

Status ASTVisitorImpl::ProcessImport(const pypa::AstImportPtr& import) {
  auto alias = PYPA_PTR_CAST(Alias, import->names);
  std::string name = PYPA_PTR_CAST(Name, alias->name)->id;
  std::string as_name = AsNameFromAlias(alias);

  if (!module_handler_->contains(name)) {
    return CreateAstError(import, "ModuleNotFoundError: No module named '$0'", name);
  }
  var_table_->Add(as_name, (*module_handler_)[name]);
  return Status::OK();
}

Status ASTVisitorImpl::ProcessImportFrom(const pypa::AstImportFromPtr& from) {
  if (from->level != 0) {
    return error::Unimplemented("Unexpected import level $0, expected 0", from->level);
  }
  std::string module = PYPA_PTR_CAST(Name, from->module)->id;

  if (!module_handler_->contains(module)) {
    return CreateAstError(from, "ModuleNotFoundError: No module named '$0'", module);
  }
  auto obj = (*module_handler_)[module];

  pypa::AstExprList aliases;
  if (from->names->type == AstType::Tuple) {
    auto tup = PYPA_PTR_CAST(Tuple, from->names);
    aliases = tup->elements;
  } else if (from->names->type == AstType::Alias) {
    aliases = {from->names};
  } else {
    return CreateAstError(from->names, "Unexpected type in import statement '$0'",
                          magic_enum::enum_name(from->names->type));
  }
  for (const auto& el : aliases) {
    auto alias = PYPA_PTR_CAST(Alias, el);
    std::string name = PYPA_PTR_CAST(Name, alias->name)->id;
    std::string as_name = AsNameFromAlias(alias);

    if (!obj->HasAttribute(name)) {
      return CreateAstError(from, "cannot import name '$1' from '$0'", module, name);
    }
    PL_ASSIGN_OR_RETURN(auto attr, obj->GetAttribute(from, name));
    var_table_->Add(as_name, attr);
  }
  return Status::OK();
}

// Assignment by subscript is more restrictive than assignment by attribute.
// Subscript assignment is currently only valid for creating map expressions such as the
// following: df['foo'] = 1+2
Status ASTVisitorImpl::ProcessSubscriptAssignment(const pypa::AstSubscriptPtr& subscript,
                                                  const pypa::AstExpr& expr_node) {
  PL_ASSIGN_OR_RETURN(auto processed_node, Process(subscript, {{}, "", {}}));
  PL_ASSIGN_OR_RETURN(auto processed_target_table, Process(subscript->value, {{}, "", {}}));

  if (processed_target_table->type() != QLObjectType::kDataframe) {
    return CreateAstError(subscript, "Can't assign to node via subscript of type $0",
                          processed_target_table->name());
  }

  return ProcessMapAssignment(subscript->value,
                              std::static_pointer_cast<Dataframe>(processed_target_table),
                              processed_node, expr_node);
}

// Assignment by attribute supports all of the cases that subscript assignment does,
// in addition to assigning to
Status ASTVisitorImpl::ProcessAttributeAssignment(const pypa::AstAttributePtr& attr,
                                                  const pypa::AstExpr& expr_node) {
  PL_ASSIGN_OR_RETURN(auto processed_target, Process(attr->value, {{}, "", {}}));

  if (processed_target->type() != QLObjectType::kDataframe) {
    PL_ASSIGN_OR_RETURN(std::string attr_name, GetAttributeStr(attr));
    PL_ASSIGN_OR_RETURN(auto processed_value,
                        Process(PYPA_PTR_CAST(Call, expr_node), {{}, "", {}}));
    return processed_target->AssignAttribute(attr_name, processed_value);
  }

  // If the target is a Dataframe, we are doing a Subscript map assignment like "df.foo = 2".
  // We need to do special handling here as opposed to the above logic in order to produce a new
  // dataframe.
  PL_ASSIGN_OR_RETURN(auto processed_node, Process(attr, {{}, "", {}}));
  return ProcessMapAssignment(attr->value, std::static_pointer_cast<Dataframe>(processed_target),
                              processed_node, expr_node);
}

Status ASTVisitorImpl::ProcessMapAssignment(const pypa::AstPtr& assign_target,
                                            std::shared_ptr<Dataframe> parent_df,
                                            QLObjectPtr target_node,
                                            const pypa::AstExpr& expr_node) {
  if (assign_target->type != AstType::Name) {
    return CreateAstError(assign_target,
                          "Can only assign to Dataframe by subscript from Name, received $0",
                          GetAstTypeName(assign_target->type));
  }
  auto assign_name_string = GetNameAsString(assign_target);
  PL_ASSIGN_OR_RETURN(ColumnIR * target_column,
                      GetArgAs<ColumnIR>(target_node, "assignment value"));

  if (!parent_df->op()) {
    return CreateAstError(assign_target,
                          "Cannot assign column to dataframe that does not contain an operator");
  }

  // Maps can only assign to the same table as the input table when of the form:
  // df['foo'] = df['bar'] + 2
  OperatorContext op_context{{parent_df->op()}, Dataframe::kMapOpId, {assign_name_string}};
  PL_ASSIGN_OR_RETURN(auto expr_obj, Process(expr_node, op_context));
  PL_ASSIGN_OR_RETURN(ExpressionIR * expr_val,
                      GetArgAs<ExpressionIR>(expr_obj, "assignment value"));

  PL_ASSIGN_OR_RETURN(auto dataframe,
                      parent_df->FromColumnAssignment(expr_node, target_column, expr_val));
  var_table_->Add(assign_name_string, dataframe);

  return ir_graph_->DeleteNode(target_column->id());
}

StatusOr<QLObjectPtr> ASTVisitorImpl::Process(const pypa::AstExpr& node,
                                              const OperatorContext& op_context) {
  switch (node->type) {
    case AstType::Call:
      return ProcessCallNode(PYPA_PTR_CAST(Call, node), op_context);
    case AstType::Subscript:
      return ProcessSubscriptCall(PYPA_PTR_CAST(Subscript, node), op_context);
    case AstType::Name:
      return LookupVariable(PYPA_PTR_CAST(Name, node));
    case AstType::Attribute:
      return ProcessAttribute(PYPA_PTR_CAST(Attribute, node), op_context);
    case AstType::Str: {
      return ProcessStr(PYPA_PTR_CAST(Str, node));
    }
    case AstType::Number: {
      return ProcessNumber(PYPA_PTR_CAST(Number, node));
    }
    case AstType::List: {
      return ProcessList(PYPA_PTR_CAST(List, node), op_context);
    }
    case AstType::Tuple: {
      return ProcessTuple(PYPA_PTR_CAST(Tuple, node), op_context);
    }
    case AstType::BinOp: {
      return ProcessDataBinOp(PYPA_PTR_CAST(BinOp, node), op_context);
    }
    case AstType::BoolOp: {
      return ProcessDataBoolOp(PYPA_PTR_CAST(BoolOp, node), op_context);
    }
    case AstType::Compare: {
      return ProcessDataCompare(PYPA_PTR_CAST(Compare, node), op_context);
    }
    case AstType::UnaryOp: {
      return ProcessDataUnaryOp(PYPA_PTR_CAST(UnaryOp, node), op_context);
    }
    default:
      return CreateAstError(node, "Expression node '$0' not defined", GetAstTypeName(node->type));
  }
}

Status ASTVisitorImpl::ProcessAssignNode(const pypa::AstAssignPtr& node) {
  // Check # nodes to assign.
  if (node->targets.size() != 1) {
    return CreateAstError(node, "We only support single target assignment.");
  }
  // Get the name that we are targeting.
  auto target_node = node->targets[0];

  // Special handler for this type of statement: df['foo'] = df['bar']
  if (target_node->type == AstType::Subscript) {
    return ProcessSubscriptAssignment(PYPA_PTR_CAST(Subscript, node->targets[0]), node->value);
  }
  if (target_node->type == AstType::Attribute) {
    return ProcessAttributeAssignment(PYPA_PTR_CAST(Attribute, node->targets[0]), node->value);
  }

  if (target_node->type != AstType::Name) {
    return CreateAstError(target_node, "Assignment target must be a Name or Subscript");
  }

  std::string assign_name = GetNameAsString(target_node);
  OperatorContext op_context({}, "", {});
  PL_ASSIGN_OR_RETURN(auto processed_node, Process(node->value, op_context));
  var_table_->Add(assign_name, processed_node);
  return Status::OK();
}

Status ASTVisitorImpl::DoesArgMatchAnnotation(QLObjectPtr ql_arg, QLObjectPtr annotation_obj) {
  DCHECK(annotation_obj);
  DCHECK(ql_arg->HasNode());
  auto arg = ql_arg->node();
  if (annotation_obj->type() == QLObjectType::kType) {
    auto type_object = std::static_pointer_cast<TypeObject>(annotation_obj);
    if (!arg->IsExpression()) {
      return arg->CreateIRNodeError("Expected '$0', received '$1'", type_object->TypeString(),
                                    arg->type_string());
    }
    return type_object->NodeMatches(static_cast<ExpressionIR*>(arg));
  } else if (annotation_obj->type() != ql_arg->type()) {
    return arg->CreateIRNodeError("Expected '$0', received '$1'", annotation_obj->name(),
                                  ql_arg->name());
  }
  return Status::OK();
}

StatusOr<QLObjectPtr> ASTVisitorImpl::FuncDefHandler(
    const std::vector<std::string>& arg_names,
    const absl::flat_hash_map<std::string, QLObjectPtr>& arg_annotation_objs,
    const pypa::AstSuitePtr& body, const pypa::AstPtr& ast, const ParsedArgs& args) {
  // TODO(philkuz) (PL-1365) figure out how to wrap the internal errors with the ast that's passed
  // in.
  PL_UNUSED(ast);

  auto func_visitor = CreateChild();
  for (const std::string& arg_name : arg_names) {
    QLObjectPtr arg_object = args.GetArg(arg_name);

    if (arg_annotation_objs.contains(arg_name)) {
      PL_RETURN_IF_ERROR(
          DoesArgMatchAnnotation(arg_object, arg_annotation_objs.find(arg_name)->second));
    }
    func_visitor->var_table()->Add(arg_name, arg_object);
  }

  return func_visitor->ProcessASTSuite(body, /*is_function_definition_body*/ true);
}

Status ASTVisitorImpl::ProcessFunctionDefNode(const pypa::AstFunctionDefPtr& node) {
  // Create the func object.
  // Use the new function defintion body as the function object.
  // Every time the function is evaluated we should evlauate the body with the values for the
  // args passed into the scope.
  // Parse the args to create the necessary
  auto function_name_node = node->name;
  std::vector<std::string> parsed_arg_names;
  absl::flat_hash_map<std::string, QLObjectPtr> arg_annotations_objs;
  for (const auto& arg : node->args.arguments) {
    if (arg->type != AstType::Arg) {
      return CreateAstError(arg, "function parameter must be an argument, not a $0",
                            GetAstTypeName(arg->type));
    }
    auto arg_ptr = PYPA_PTR_CAST(Arg, arg);
    parsed_arg_names.push_back(arg_ptr->arg);
    if (arg_ptr->annotation) {
      PL_ASSIGN_OR_RETURN(auto annotation_obj, Process(arg_ptr->annotation, {{}, "", {}}));
      arg_annotations_objs[arg_ptr->arg] = annotation_obj;
    }
  }
  // TODO(philkuz) delete keywords from the function definition.
  // For some reason this is kept around, not clear why, making sure that it's 0 for now.
  DCHECK_EQ(node->args.keywords.size(), 0UL);

  // The default values for args. Should be the same length as args. For now we should consider
  // not processing these.
  DCHECK_EQ(node->args.defaults.size(), node->args.arguments.size());
  for (const auto& default_value : node->args.defaults) {
    // TODO(philkuz) support default.
    if (default_value != nullptr) {
      return CreateAstError(default_value, "default values not supported in function definitions");
    }
  }

  // TODO(philkuz) support *args.
  if (node->args.args) {
    return CreateAstError(node->args.args,
                          "variable length args are not supported in function definitions");
  }

  // TODO(philkuz) support **kwargs
  if (node->args.kwargs) {
    return CreateAstError(node->args.kwargs,
                          "variable length kwargs are not supported in function definitions");
  }

  if (function_name_node->type != AstType::Name) {
    return CreateAstError(function_name_node, "function definition must be a name, not a $0",
                          GetAstTypeName(function_name_node->type));
  }

  if (node->body->type != AstType::Suite) {
    return CreateAstError(node->body, "function body of type $0 not allowed",
                          GetAstTypeName(node->body->type));
  }
  pypa::AstSuitePtr body = PYPA_PTR_CAST(Suite, node->body);
  std::string function_name = GetNameAsString(function_name_node);

  PL_ASSIGN_OR_RETURN(auto defined_func,
                      FuncObject::Create(function_name, parsed_arg_names, {}, false, false,
                                         std::bind(&ASTVisitorImpl::FuncDefHandler, this,
                                                   parsed_arg_names, arg_annotations_objs, body,
                                                   std::placeholders::_1, std::placeholders::_2),
                                         this));
  DCHECK_LE(node->decorators.size(), 1);
  for (const auto& d : node->decorators) {
    // Each decorator should be a function that takes in the defined_func as an argument.
    PL_ASSIGN_OR_RETURN(auto dec_fn, Process(d, OperatorContext{{}, ""}));
    PL_ASSIGN_OR_RETURN(auto fn_object, GetCallMethod(d, dec_fn));
    ArgMap map{{}, {defined_func}};
    PL_ASSIGN_OR_RETURN(auto object_fn, fn_object->Call(map, d));
    PL_ASSIGN_OR_RETURN(defined_func, GetCallMethod(d, object_fn));
  }

  PL_ASSIGN_OR_RETURN(auto doc_string, ProcessFuncDefDocString(body));
  PL_RETURN_IF_ERROR(defined_func->AddDocString(doc_string));

  PL_RETURN_IF_ERROR(defined_func->ResolveArgAnnotationsToTypes(arg_annotations_objs));

  var_table_->Add(function_name, defined_func);
  return Status::OK();
}

Status ASTVisitorImpl::ValidateSubscriptValue(const pypa::AstExpr& node,
                                              const OperatorContext& op_context) {
  if (op_context.operator_name != Dataframe::kMapOpId) {
    return Status::OK();
  }
  switch (node->type) {
    case AstType::Attribute: {
      // We want to make sure that the parent of an attribute is completely valid, even if it's
      // nested. ie. `df.ctx['service']`
      return ValidateSubscriptValue(PYPA_PTR_CAST(Attribute, node)->value, op_context);
    }
    case AstType::Name: {
      std::string name = GetNameAsString(node);
      if (std::find(op_context.referenceable_dataframes.begin(),
                    op_context.referenceable_dataframes.end(),
                    name) == op_context.referenceable_dataframes.end()) {
        return CreateAstError(node, "name '$0' is not available in this context", name);
      }
      ABSL_FALLTHROUGH_INTENDED;
    }
    default:
      return Status::OK();
  }
}
StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessSubscriptCall(const pypa::AstSubscriptPtr& node,
                                                           const OperatorContext& op_context) {
  // Validate to make sure that we can actually take the subscript in this context.
  PL_RETURN_IF_ERROR(ValidateSubscriptValue(node->value, op_context));
  PL_ASSIGN_OR_RETURN(QLObjectPtr pyobject, Process(node->value, op_context));
  if (!pyobject->HasSubscriptMethod()) {
    return pyobject->CreateError("$0 is not subscriptable", pyobject->name());
  }
  PL_ASSIGN_OR_RETURN(std::shared_ptr<FuncObject> func_object, pyobject->GetSubscriptMethod());

  auto slice = node->slice;
  if (slice->type != AstType::Index) {
    return CreateAstError(slice, "'$0' object cannot be an index", GetAstTypeName(slice->type));
  }

  std::vector<std::string> dfs = op_context.referenceable_dataframes;
  if (node->value->type == AstType::Name) {
    dfs.push_back(GetNameAsString(node->value));
  }

  OperatorContext new_op_context(op_context.parent_ops, op_context.operator_name, dfs);
  PL_ASSIGN_OR_RETURN(QLObjectPtr arg, Process(PYPA_PTR_CAST(Index, slice)->value, new_op_context));
  ArgMap args;
  args.args.push_back(arg);
  return func_object->Call(args, node);
}

StatusOr<std::string> ASTVisitorImpl::GetFuncName(const pypa::AstCallPtr& node) {
  std::string func_name;
  switch (node->function->type) {
    case AstType::Name: {
      func_name = GetNameAsString(node->function);
      break;
    }
    case AstType::Attribute: {
      auto attr = PYPA_PTR_CAST(Attribute, node->function);
      if (attr->attribute->type != AstType::Name) {
        return CreateAstError(node->function, "Couldn't get string name out of node of type $0.",
                              GetAstTypeName(attr->attribute->type));
      }
      func_name = GetNameAsString(attr->attribute);
      break;
    }
    default: {
      return CreateAstError(node->function, "Couldn't get string name out of node of type $0.",
                            GetAstTypeName(node->function->type));
    }
  }
  return func_name;
}

StatusOr<ArgMap> ASTVisitorImpl::ProcessArgs(const pypa::AstCallPtr& call_ast,
                                             const OperatorContext& op_context) {
  ArgMap arg_map;
  for (const auto arg : call_ast->arguments) {
    PL_ASSIGN_OR_RETURN(auto value, Process(arg, op_context));
    arg_map.args.push_back(value);
  }

  // Iterate through the keywords
  for (auto& kw_ptr : call_ast->keywords) {
    std::string key = GetNameAsString(kw_ptr->name);
    PL_ASSIGN_OR_RETURN(auto value, Process(kw_ptr->value, op_context));
    arg_map.kwargs.emplace_back(key, value);
  }

  return arg_map;
}

StatusOr<QLObjectPtr> ASTVisitorImpl::LookupVariable(const pypa::AstPtr& ast,
                                                     const std::string& name) {
  auto var = var_table_->Lookup(name);
  if (var == nullptr) {
    return CreateAstError(ast, "name '$0' is not defined", name);
  }
  return var;
}

StatusOr<OperatorIR*> ASTVisitorImpl::LookupName(const pypa::AstNamePtr& name_node) {
  PL_ASSIGN_OR_RETURN(QLObjectPtr pyobject, LookupVariable(name_node));
  if (!pyobject->HasNode()) {
    return CreateAstError(name_node, "'$0' not accessible", name_node->id);
  }
  IRNode* node = pyobject->node();
  if (!node->IsOperator()) {
    return node->CreateIRNodeError("Only dataframes may be assigned variables, $0 not allowed",
                                   node->type_string());
  }
  return static_cast<OperatorIR*>(node);
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessAttribute(const pypa::AstAttributePtr& node,
                                                       const OperatorContext& op_context) {
  PL_ASSIGN_OR_RETURN(std::string attr_name, GetAttributeStr(node));
  PL_ASSIGN_OR_RETURN(QLObjectPtr value_obj, Process(node->value, op_context));
  return value_obj->GetAttribute(node, attr_name);
}

StatusOr<std::string> ASTVisitorImpl::GetAttributeStr(const pypa::AstAttributePtr& attr) {
  if (attr->attribute->type != AstType::Name) {
    return CreateAstError(attr, "$0 not a valid attribute", GetAstTypeName(attr->attribute->type));
  }
  return GetNameAsString(attr->attribute);
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessCallNode(const pypa::AstCallPtr& node,
                                                      const OperatorContext& op_context) {
  std::shared_ptr<FuncObject> func_object;
  // pyobject declared up here because we need this object to be allocated when
  // func_object->Call() is made.
  PL_ASSIGN_OR_RETURN(QLObjectPtr pyobject, Process(node->function, op_context));
  if (pyobject->type_descriptor().type() == QLObjectType::kExpr) {
    if (Match(pyobject->node(), ColumnNode())) {
      return CreateAstError(node, "dataframe has no method '$0'",
                            static_cast<ColumnIR*>(pyobject->node())->col_name());
    }
    return CreateAstError(node, "expression object is not callable");
  }

  PL_ASSIGN_OR_RETURN(func_object, GetCallMethod(node, pyobject));
  PL_ASSIGN_OR_RETURN(ArgMap args, ProcessArgs(node, op_context));
  return func_object->Call(args, node);
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessStr(const pypa::AstStrPtr& ast) {
  PL_ASSIGN_OR_RETURN(auto str_value, GetStrAstValue(ast));
  PL_ASSIGN_OR_RETURN(StringIR * node, ir_graph_->CreateNode<StringIR>(ast, str_value));
  return ExprObject::Create(node, this);
}

StatusOr<std::vector<QLObjectPtr>> ASTVisitorImpl::ProcessCollectionChildren(
    const pypa::AstExprList& elements, const OperatorContext& op_context) {
  std::vector<QLObjectPtr> children;
  for (auto& child : elements) {
    PL_ASSIGN_OR_RETURN(QLObjectPtr child_node, Process(child, op_context));
    children.push_back(child_node);
  }
  return children;
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessList(const pypa::AstListPtr& ast,
                                                  const OperatorContext& op_context) {
  PL_ASSIGN_OR_RETURN(std::vector<QLObjectPtr> expr_vec,
                      ProcessCollectionChildren(ast->elements, op_context));
  return ListObject::Create(expr_vec, this);
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessTuple(const pypa::AstTuplePtr& ast,
                                                   const OperatorContext& op_context) {
  PL_ASSIGN_OR_RETURN(std::vector<QLObjectPtr> expr_vec,
                      ProcessCollectionChildren(ast->elements, op_context));
  return TupleObject::Create(expr_vec, this);
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessNumber(const pypa::AstNumberPtr& node) {
  switch (node->num_type) {
    case pypa::AstNumber::Type::Float: {
      PL_ASSIGN_OR_RETURN(FloatIR * ir_node, ir_graph_->CreateNode<FloatIR>(node, node->floating));
      return ExprObject::Create(ir_node, this);
    }
    case pypa::AstNumber::Type::Integer:
    case pypa::AstNumber::Type::Long: {
      PL_ASSIGN_OR_RETURN(IntIR * ir_node, ir_graph_->CreateNode<IntIR>(node, node->integer));
      return ExprObject::Create(ir_node, this);
    }
    default:
      return CreateAstError(node, "Couldn't find number type $0", node->num_type);
  }
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessDataBinOp(const pypa::AstBinOpPtr& node,
                                                       const OperatorContext& op_context) {
  std::string op_str = pypa::to_string(node->op);

  PL_ASSIGN_OR_RETURN(auto left_obj, Process(node->left, op_context));
  PL_ASSIGN_OR_RETURN(auto right_obj, Process(node->right, op_context));
  PL_ASSIGN_OR_RETURN(ExpressionIR * left,
                      GetArgAs<ExpressionIR>(left_obj, "left side of operation"));
  PL_ASSIGN_OR_RETURN(ExpressionIR * right,
                      GetArgAs<ExpressionIR>(right_obj, "right side of operation"));

  PL_ASSIGN_OR_RETURN(FuncIR::Op op, GetOp(op_str, node));
  std::vector<ExpressionIR*> args = {left, right};
  PL_ASSIGN_OR_RETURN(FuncIR * ir_node, ir_graph_->CreateNode<FuncIR>(node, op, args));
  return ExprObject::Create(ir_node, this);
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessDataBoolOp(const pypa::AstBoolOpPtr& node,
                                                        const OperatorContext& op_context) {
  std::string op_str = pypa::to_string(node->op);
  if (node->values.size() != 2) {
    return CreateAstError(node, "Expected two arguments to '$0'.", op_str);
  }

  PL_ASSIGN_OR_RETURN(auto left_obj, Process(node->values[0], op_context));
  PL_ASSIGN_OR_RETURN(auto right_obj, Process(node->values[1], op_context));
  PL_ASSIGN_OR_RETURN(ExpressionIR * left,
                      GetArgAs<ExpressionIR>(left_obj, "left side of operation"));
  PL_ASSIGN_OR_RETURN(ExpressionIR * right,
                      GetArgAs<ExpressionIR>(right_obj, "right side of operation"));

  PL_ASSIGN_OR_RETURN(FuncIR::Op op, GetOp(op_str, node));
  std::vector<ExpressionIR*> args{left, right};
  PL_ASSIGN_OR_RETURN(FuncIR * ir_node, ir_graph_->CreateNode<FuncIR>(node, op, args));
  return ExprObject::Create(ir_node, this);
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessDataCompare(const pypa::AstComparePtr& node,
                                                         const OperatorContext& op_context) {
  DCHECK_EQ(node->operators.size(), 1ULL);
  std::string op_str = pypa::to_string(node->operators[0]);
  if (node->comparators.size() != 1) {
    return CreateAstError(node, "Only expected one argument to the right of '$0'.", op_str);
  }

  PL_ASSIGN_OR_RETURN(auto left_obj, Process(node->left, op_context));
  PL_ASSIGN_OR_RETURN(ExpressionIR * left,
                      GetArgAs<ExpressionIR>(left_obj, "left side of operation"));
  std::vector<ExpressionIR*> expressions{left};

  for (const auto& comp : node->comparators) {
    PL_ASSIGN_OR_RETURN(auto obj, Process(comp, op_context));
    PL_ASSIGN_OR_RETURN(ExpressionIR * expr, GetArgAs<ExpressionIR>(obj, "argument to operation"));
    expressions.push_back(expr);
  }

  PL_ASSIGN_OR_RETURN(FuncIR::Op op, GetOp(op_str, node));
  PL_ASSIGN_OR_RETURN(FuncIR * ir_node, ir_graph_->CreateNode<FuncIR>(node, op, expressions));
  return ExprObject::Create(ir_node, this);
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessDataUnaryOp(const pypa::AstUnaryOpPtr& node,
                                                         const OperatorContext& op_context) {
  PL_ASSIGN_OR_RETURN(auto operand_obj, Process(node->operand, op_context));
  PL_ASSIGN_OR_RETURN(ExpressionIR * operand,
                      GetArgAs<ExpressionIR>(operand_obj, "operand of unary op"));

  std::string op_str = pypa::to_string(node->op);
  PL_ASSIGN_OR_RETURN(FuncIR::Op op, GetUnaryOp(op_str, node));
  if (op.op_code == FuncIR::Opcode::non_op) {
    return ExprObject::Create(operand, this);
  }
  std::vector<ExpressionIR*> args{operand};
  PL_ASSIGN_OR_RETURN(FuncIR * ir_node, ir_graph_->CreateNode<FuncIR>(node, op, args));
  return ExprObject::Create(ir_node, this);
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessFuncDefReturn(const pypa::AstReturnPtr& ret) {
  if (ret->value == nullptr) {
    return std::static_pointer_cast<QLObject>(std::make_shared<NoneObject>(ret, this));
  }

  return Process(ret->value, {{}, "", {}});
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessDocString(const pypa::AstDocStringPtr& doc_string) {
  PL_ASSIGN_OR_RETURN(StringIR * ir_node,
                      ir_graph_->CreateNode<StringIR>(doc_string, doc_string->doc));
  return ExprObject::Create(ir_node, this);
}

StatusOr<QLObjectPtr> ASTVisitorImpl::ProcessFuncDefDocString(const pypa::AstSuitePtr& body) {
  pypa::AstStmtList items_list = body->items;
  if (items_list.size() == 0 || items_list[0]->type != pypa::AstType::DocString) {
    PL_ASSIGN_OR_RETURN(StringIR * ir_node, ir_graph_->CreateNode<StringIR>(body, ""));
    return ExprObject::Create(ir_node, this);
  }
  return ProcessDocString(PYPA_PTR_CAST(DocString, items_list[0]));
}

StatusOr<VisFuncsInfo> ASTVisitorImpl::GetVisFuncsInfo() const {
  VisFuncsInfo info;
  auto doc_string_map = info.mutable_doc_string_map();
  auto vis_spec_map = info.mutable_vis_spec_map();
  auto fn_args_map = info.mutable_fn_args_map();
  for (const auto& [name, func] : var_table_->GetVisFuncs()) {
    doc_string_map->insert({name, func->doc_string()});
    auto vis_spec = func->vis_spec();
    pl::shared::scriptspb::VisSpec vis_spec_pb;
    vis_spec_pb.set_vega_spec(vis_spec->vega_spec);
    vis_spec_map->insert({name, vis_spec_pb});
    fn_args_map->insert({name, func->CreateFuncArgsSpec()});
  }
  return info;
}

}  // namespace compiler
}  // namespace planner
}  // namespace carnot
}  // namespace pl
