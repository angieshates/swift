//===--- TypeChecker.h - Type Checking Class --------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file defines the TypeChecking class.
//
//===----------------------------------------------------------------------===//

#ifndef TYPECHECKING_H
#define TYPECHECKING_H

#include "swift/AST/AST.h"
#include "swift/AST/ASTMutationListener.h"
#include "swift/AST/Diagnostics.h"
#include "swift/Basic/Fallthrough.h"
#include "llvm/ADT/SetVector.h"
#include <functional>

namespace swift {

class MemberLookup;
class TypeChecker;

/// CoercionResult - Describes the result of attempting to coerce an
/// expression to a given type.
enum class CoercionResult {
  Succeeded,
  Failed,
  Unknowable
};

/// CoercedExpr - Describes the result of coercing an expression to a given
/// type.
class CoercedExpr {
  TypeChecker &TC;
  CoercionResult Kind;
  Expr *E;
  Type DestTy;

  void diagnose() const;

public:
  CoercedExpr(TypeChecker &TC, CoercionResult Kind, Expr *E, Type DestTy)
    : TC(TC), Kind(Kind), E(E), DestTy(DestTy) {}

  CoercionResult getKind() const { return Kind; }
  Expr *getExpr() const { return E; }

  explicit operator bool() const { return Kind == CoercionResult::Succeeded; }

  operator Expr*() const {
    switch (Kind) {
    case CoercionResult::Succeeded:
      return E;

    case CoercionResult::Unknowable:
      diagnose();
      SWIFT_FALLTHROUGH;

    case CoercionResult::Failed:
      return 0;
    }
  }
};

/// \brief Describes the kind of a literal.
enum class LiteralKind : char {
  Int, Float, Char, UTFString, ASCIIString, Array, Dictionary
};

/// \brief A mapping from substitutable types to the protocol-conformance
/// mappings for those types.
typedef llvm::DenseMap<SubstitutableType *,
                       SmallVector<ProtocolConformance *, 2>> ConformanceMap;

/// \brief An overload candidate.
/// FIXME: Encode the actual substitutions here in some efficient manner.
class OverloadCandidate {
public:
  /// SubstitutionInfoType - Information about the substitution of generic
  /// parameters required to produce this overload candidate.
  struct SubstitutionInfoType {
    TypeSubstitutionMap Substitutions;
    ConformanceMap Conformances;
  };

private:
  llvm::PointerIntPair<ValueDecl *, 1, bool> DeclAndComplete;
  Type Ty;
  std::unique_ptr<SubstitutionInfoType> SubstitutionInfo;
  Type InferredBaseTy;
  
public:
  OverloadCandidate() : DeclAndComplete(0, false), Ty() { }
  OverloadCandidate(ValueDecl *Value, Type Ty, bool Complete)
    : DeclAndComplete(Value, Complete), Ty(Ty) { }
  OverloadCandidate(ValueDecl *Value, Type Ty, Type InferredBaseTy)
    : DeclAndComplete(Value, true), Ty(Ty), InferredBaseTy(InferredBaseTy){}
  OverloadCandidate(ValueDecl *Value, Type Ty,
                    SubstitutionInfoType &&SubstitutionInfo)
    : DeclAndComplete(Value, true), Ty(Ty),
      SubstitutionInfo(new SubstitutionInfoType(std::move(SubstitutionInfo))) {}

  /// \brief Whether this overload candidate is 'complete', meaning that we
  /// can use the declaration (possibly by applying the given substitutions)
  /// immediately.
  bool isComplete() const { return DeclAndComplete.getInt(); }

  /// \brief Retrieve the declaration 
  ValueDecl *getDecl() const { return DeclAndComplete.getPointer(); }

  /// \brief Set the declaration.
  void setDecl(ValueDecl *D) { DeclAndComplete.setPointer(D); }

  /// \brief Retrieve the type of a reference to this overload candidate,
  /// after substitution.
  Type getType() const { return Ty; }

  /// \brief Retrieve the inferred base type to be used when referencing
  /// this declaration.
  ///
  /// This inferred base type is used when we're performing operator lookup
  /// into, e.g., protocols, and we need to record the inferred type of 'This'.
  Type getInferredBaseType() const { return InferredBaseTy; }

  /// \brief Evaluates true if the selected candidate is complete.
  explicit operator bool() const { return isComplete(); }

  /// \brief Determine whether this overload candidate has any substitutions.
  bool hasSubstitutions() const { return SubstitutionInfo != nullptr; }

  /// \brief Retrieve the substitutions for this overload candidate.
  TypeSubstitutionMap &getSubstitutions() const {
    assert(hasSubstitutions() && "Candidate does not have substitutions");
    return SubstitutionInfo->Substitutions;
  }

  /// \brief Retrieve the set of protocol conformance records that go with the
  /// substitutions.
  ConformanceMap &getConformances() const {
    assert(hasSubstitutions() && "Candidate does not have substitutions");
    return SubstitutionInfo->Conformances;
  }
};

/// \brief Describes the kind of coercion being performed.
enum class CoercionKind {
  // Normal coercion to the given type.
  Normal,
  // Coercion of a tuple when calling an assignment operator, which permits
  // the first operand to be an implicit lvalue.
  Assignment,
  // Coercion to an implicit lvalue.
  ImplicitLValue
};

/// \brief Captures information about the context of a type coercion, including
/// the type checker instance and the type substitutions deduced by the
/// coercion.
struct CoercionContext {
  CoercionContext(TypeChecker &TC) : TC(TC) { }
  
  TypeChecker &TC;
  TypeSubstitutionMap Substitutions;

  /// \brief Mapping from each of the substitutable types to the set of
  /// protocol-conformance mappings for each of the requirements on the
  /// type.
  ConformanceMap Conformance;

  /// \brief Identify the set of generic parameters for which we want to
  /// compute substitutions.
  void requestSubstitutionsFor(ArrayRef<DeducibleGenericParamType *> Params);
  
  /// \brief Retrieve the substitution for the given deducible generic
  /// parameter type.
  Type getSubstitution(DeducibleGenericParamType *Deducible) const {
    TypeSubstitutionMap::const_iterator Pos = Substitutions.find(Deducible);
    assert(Pos != Substitutions.end() && "Not deducible");
    return Pos->second;
  }

  /// \brief Determine whether the given coercion context requires
  /// substitutions.
  bool requiresSubstitution() const { return !Substitutions.empty(); }

  /// \brief Determine whether this coercion context has complete substitution
  /// information.
  bool hasCompleteSubstitutions() const;
};

/// \brief Wraps an expression that refers to an overloaded set of declarations,
/// which may have various syntactic forms (normal reference, member reference,
/// operator name) and may also refer to a single, generic declaration.
///
/// This class is used when decomposing an expression into the pieces used
/// by overload resolution.
class OverloadedExpr {
  /// \brief The actual expression, along with a bit that indicates whether
  /// this is an overloaded member operation
  llvm::PointerIntPair<Expr *, 1, bool> E;

  union {
    struct {
      ValueDecl * const *Start;
      unsigned Size;
    } Overloaded;

    struct {
      ValueDecl *Single;
    } Generic;
  };

  /// \brief The type of the base of a member access, if in fact this is a
  /// member access.
  Type BaseTy;

public:
  /// \brief Creates an empty overloaded expression.
  OverloadedExpr() : E(nullptr, false) { }

  /// \brief Creates an overloaded expression that refers to a single (generic)
  /// entity.
  OverloadedExpr(Expr *E, Type BaseTy, ValueDecl *Single)
    : E(E, false), BaseTy(BaseTy)
  {
    Generic.Single = Single;
  }

  /// \brief Creates an overloaded expression that refers to a set of
  /// declarations.
  OverloadedExpr(Expr *E, Type BaseTy, ArrayRef<ValueDecl *> Candidates)
    : E(E, true), BaseTy(BaseTy)
  {
    Overloaded.Start = Candidates.data();
    Overloaded.Size = Candidates.size();
  }

  /// \brief Creates an overloaded expression from an overload set expression.
  /// FIXME: This constructor is meant to be temporary; client should go
  /// through TypeChecker::getOverloadExpr() instead.
  OverloadedExpr(OverloadSetRefExpr *OSE)
    : E(OSE, true), BaseTy(OSE->getBaseType())
  {
    Overloaded.Start = OSE->getDecls().data();
    Overloaded.Size = OSE->getDecls().size();
  }

  /// \brief Determine if this overloaded expression stores a valid expression.
  explicit operator bool() const { return E.getPointer(); }

  /// \brief Retrieve the overloaded expression.
  Expr *getExpr() const { return E.getPointer(); }

  /// \brief Retrieve the type of the base object, or a null type if there is
  /// no base.
  Type getBaseType() const { return BaseTy; }

  /// \brief Retrieve the set of overload candidates, which may contain a
  /// single declaration (if it is generic).
  ArrayRef<ValueDecl *> getCandidates() const {
    if (E.getInt()) {
      return ArrayRef<ValueDecl *>(Overloaded.Start, Overloaded.Size);
    }

    return ArrayRef<ValueDecl *>(&Generic.Single, 1);
  }
};

/// \brief The set of known protocols.
enum class KnownProtocolKind : unsigned {
  /// \brief The 'ArrayBound' protocol, used for array bounds.
  ArrayBound,

  /// \brief The 'ArrayLiteralConvertible' protocol, used for array literals.
  ArrayLiteralConvertible,

  /// \brief The 'DictionaryLiteralConvertible' protocol, used for dictionary
  /// literals.
  DictionaryLiteralConvertible,

  /// \brief The 'Enumerable' protocol, used by the for-each loop.
  Enumerable,

  /// \brief The 'Enumerator' protocol, used by the for-each loop.
  Enumerator,

  /// \brief The 'LogicValue' protocol, used for places where a value is
  /// considered to be a logic value, such as in an 'if' statement.
  LogicValue,

  /// \brief The 'StringInterpolationConvertible ' protocol, used for string
  /// interpolation literals.
  StringInterpolationConvertible
};

class TypeChecker : public ASTMutationListener {
public:
  TranslationUnit &TU;
  ASTContext &Context;

  /// \brief The list of implicitly-defined functions created by the
  /// type checker.
  std::vector<FuncExprLike> implicitlyDefinedFunctions;

private:
  /// \brief The number of known protocols.
  static const unsigned numKnownProtocols
    = 1 + static_cast<unsigned>(
            KnownProtocolKind::StringInterpolationConvertible);

  /// \brief The set of known protocols, lazily populated as needed.
  ProtocolDecl *knownProtocols[numKnownProtocols] = { };

  Type IntLiteralType;
  Type FloatLiteralType;
  Type CharacterLiteralType;
  Type StringLiteralType;
  Type ArrayLiteralType;
  Type DictionaryLiteralType;
  
  DiagnosticEngine &Diags;

  /// The index of the next response metavariable to bind to a REPL result.
  unsigned NextResponseVariableIndex = 0;

public:
  TypeChecker(TranslationUnit &TU) : TypeChecker(TU, TU.Ctx.Diags) { }
  TypeChecker(TranslationUnit &TU, DiagnosticEngine &Diags);
  ~TypeChecker();

  LangOptions &getLangOpts() const { return Context.LangOpts; }
  
  template<typename ...ArgTypes>
  InFlightDiagnostic diagnose(ArgTypes... Args) {
    return Diags.diagnose(Args...);
  }

  Type getArraySliceType(SourceLoc loc, Type elementType);
  Expr *buildArrayInjectionFnRef(ArraySliceType *sliceType,
                                 Type lenTy, SourceLoc Loc);

  bool validateType(TypeLoc &Loc);

  /// \brief Validate the given type, which has no location information
  /// and shall not fail.
  /// FIXME: This concept seems a bit broken.
  void validateTypeSimple(Type T) {
    TypeLoc TL(T, SourceRange());
    bool result = validateType(TL);
    assert(!result && "Validation cannot fail!");
    (void)result;
  }

  /// \brief Transform the given type by applying the given function to
  /// each type node. If the function returns null, the transformation aborts.
  /// If it leaves the given type unchanged, then the transformation will be
  /// applied to all of the child types. If any of those types change, their
  /// parent node will be rebuilt.
  Type transformType(Type type, const std::function<Type(Type)> &fn);

  /// \brief Substitute the given archetypes for their substitution types
  /// within the given type.
  ///
  /// \returns The substituted type, or null if the substitution failed.
  ///
  /// FIXME: We probably want to have both silent and loud failure modes. However,
  /// the only possible failure now is from array slice types, which occur
  /// simply because we don't have Slice<T> yet.
  Type substType(Type T, TypeSubstitutionMap &Substitutions);

  /// \brief Replace the type \c T of a protocol member \c Member given the
  /// type of the base of a member access, \c BaseTy.
  Type substMemberTypeWithBase(Type T, ValueDecl *Member, Type BaseTy);

  /// \brief Retrieve the superclass type of the given type, or a null type if
  /// the type has no supertype.
  Type getSuperClassOf(Type type);

  bool isSubtypeOf(Type T1, Type T2, bool &Trivial,
                   CoercionContext *CC);
  bool isSubtypeOf(Type T1, Type T2, CoercionContext *CC) {
    bool Trivial = false;
    return isSubtypeOf(T1, T2, Trivial, CC);
  }
  bool isTrivialSubtypeOf(Type T1, Type T2, CoercionContext *CC = nullptr);

  /// \brief Determine whether one type is a subtype of another.
  ///
  /// \param t1 The first type.
  /// \param t2 The second type.
  ///
  /// \returns true if \c t1 is a subtype of \c t2.
  bool isSubtypeOf(Type t1, Type t2) {
    bool isTrivial;
    return isSubtypeOf(t1, t2, isTrivial);
  }

  /// \brief Determine whether one type is a subtype of another.
  ///
  /// \param t1 The first type.
  ///
  /// \param t2 The second type.
  ///
  /// \param isTrivial Will indicate whether this is a trivial subtyping
  /// relationship.
  ///
  /// \returns true if \c t1 is a subtype of \c t2.
  bool isSubtypeOfOld(Type t1, Type t2, bool &isTrivial);

  bool isSubtypeOf(Type t1, Type t2, bool &isTrivial);

  void semaFuncExpr(FuncExpr *FE, bool isFirstPass, bool allowUnknownTypes);
  bool semaTupleExpr(TupleExpr *TE);
  Expr *semaSubscriptExpr(SubscriptExpr *SE);
  Expr *semaSubscriptExpr(ExistentialSubscriptExpr *SE);
  Expr *semaSubscriptExpr(ArchetypeSubscriptExpr *SE);
  Expr *semaSubscriptExpr(GenericSubscriptExpr *SE);
  Expr *semaApplyExpr(ApplyExpr *E);
  Expr *semaUnresolvedDotExpr(UnresolvedDotExpr *E);
  
  /// If the inputs to an apply expression use a consistent "sugar" type
  /// (that is, a typealias or shorthand syntax) equivalent to the result type
  /// of the function, set the result type of the expression to that sugar type.
  Expr *substituteInputSugarTypeForResult(ApplyExpr *E);

  void typeCheckIgnoredExpr(Expr *E);
  void typeCheckFunctionBody(FuncExpr *FE);
  void typeCheckConstructorBody(ConstructorDecl *CD);
  void typeCheckDestructorBody(DestructorDecl *DD);
  void typeCheckClosureBody(PipeClosureExpr *closure);
  void typeCheckTopLevelCodeDecl(TopLevelCodeDecl *TLCD);

  void processREPLTopLevel(unsigned StartElem);
  Identifier getNextResponseVariableName();

  /// \brief Perform a shallow recheck of the given newly-built AST node.
  ///
  /// Rechecking typically occurs when one has resolved name lookup and built a
  /// new AST node that then needs to be type-checked.
  Expr *recheckTypes(Expr *E);

  void typeCheckDecl(Decl *D, bool isFirstPass);

  /// \brief Add any implicitly-defined constructors required for the given
  /// struct.
  void addImplicitConstructors(StructDecl *structDecl);

  /// \name Name lookup
  ///
  /// Routines that perform name lookup.
  ///
  /// During type checking, these routines should be used instead of
  /// \c MemberLookup and \c UnqualifiedLookup, because these routines will
  /// lazily introduce declarations and (FIXME: eventually) perform recursive
  /// type-checking that the AST-level lookup routines don't.
  ///
  /// @{
private:
  /// \brief The list of structs that require default constructors
  /// to be defined.
  ///
  /// This vector lists all structs that have implicitly-defined default
  /// constructors. Those structs not also in
  /// \c structsNeedingDefaultConstructor still need to heave their
  /// default constructors defined.
  std::vector<StructDecl *> structsWithImplicitDefaultConstructor;

  /// \brief The set of structs that still need a default constructor to be
  /// implicitly defined.
  llvm::DenseSet<StructDecl *> structsNeedingImplicitDefaultConstructor;

public:
  /// \brief Determine whether the given type can be default-initialized.
  ///
  /// \param ty The type we are checking.
  ///
  /// \param initializer If non-null, we will assigned an initializer expression
  /// that performs the default initialization.
  bool isDefaultInitializable(Type ty, Expr **initializer);

  /// \brief Define the default constructor for the given struct.
  void defineDefaultConstructor(StructDecl *structDecl);

  /// \brief Define any implicit declarations that are still pending.
  void definePendingImplicitDecls();

  /// \brief Pre-check protocol declaration, validating all of the types
  /// that are involved in conformance requirements.
  void preCheckProtocol(ProtocolDecl *D);

  /// \brief Fold the given sequence expression into an (unchecked) expression
  /// tree.
  Expr *foldSequence(SequenceExpr *expr);

  bool typeCheckExpressionOld(Expr *&E, Type ConvertType = Type());

  /// \brief Type check the given expression.
  ///
  /// \param expr The expression to type-check, which will be modified in
  /// place.
  ///
  /// \param coerceType The type that the expression is being coerced to,
  /// or null if the expression is standalone.
  ///
  /// \returns true if an error occurred, false otherwise.
  bool typeCheckExpression(Expr *&expr, Type coerceType = Type());

  /// \brief Type check the given expression as a condition, which converts
  /// it to a logic value.
  ///
  /// \param expr The expression to type-check, which will be modified in place
  /// to return a logic value (builtin i1).
  ///
  /// \returns true if an error occurred, false otherwise.
  bool typeCheckCondition(Expr *&expr);

  /// \brief Type check the given expression as an array bound, which converts
  /// it to a builtin integer value.
  ///
  /// \param expr The expression to type-check, which will be modified in
  /// place to return a builtin integral value (e.g., builtin i64).
  bool typeCheckArrayBound(Expr *&expr, bool requireConstant);

  /// \brief Type check an assignment expression using the constraint-based
  /// type checker.
  ///
  /// \param dest The destination expression.
  /// \param equalLoc The location of the '='.
  /// \param src The source expression.
  ///
  /// \returns a converted (dest, src) expression pair, or (nullptr, nullptr)
  /// if the assignment failed to type-check.
  std::pair<Expr *, Expr *> typeCheckAssignment(Expr *dest,
                                                SourceLoc equalLoc,
                                                Expr *src);

  bool typeCheckPattern(Pattern *P, bool isFirstPass, bool allowUnknownTypes);
  bool coerceToType(Pattern *P, Type Ty);
  
  bool typeCheckConditionOld(Expr *&E);
  bool typeCheckArrayBoundOld(Expr *&E);
  bool typeCheckAssignmentOld(Expr *&Dest, SourceLoc EqualLoc, Expr *&Src);


  /// \brief Compute the set of captures for the given function or closure.
  void computeCaptures(CapturingExpr *capturing);


  /// \brief Retrieve the default literal type for the given literal kind.
  Type getDefaultLiteralType(LiteralKind kind);

  /// \brief Retrieve the default literal type for the given literal
  /// expression.
  Type getDefaultLiteralType(LiteralExpr *E);

  /// resolveUnresolvedLiterals - Given an expression containing unresolved
  /// literals, resolve those unresolved literals to their default types and
  /// type-check the expression again.
  bool resolveUnresolvedLiterals(Expr *&E);

  /// coerceToType - Do semantic analysis of an expression in a context that
  /// expects a particular type.  This performs a conversion to that type if
  /// the types don't match and diagnoses cases where the conversion cannot be
  /// performed.
  ///
  /// \param Kind The kind of coercion being performed.
  ///
  /// On success, returns the coerced expression. Otherwise, returns either
  /// failure (in which case a diagnostic was produced) or 'unknowable', if
  /// it is unknown whether the coercion can occur (e.g., due to literals that
  /// have not been coerced to any specific type).
  CoercedExpr coerceToType(Expr *E, Type Ty,
                           CoercionKind Kind = CoercionKind::Normal,
                           CoercionContext *CC = nullptr);
  
  /// coerceObjectArgument - Coerce the given expression to an object argument
  /// of the given container type.
  ///
  /// The resulting expression will always be an lvalue, but may be an lvalue
  /// to a subtype of the requested container type.
  Expr *coerceObjectArgument(Expr *E, Type ContainerTy,
                             CoercionContext *CC = nullptr);
  
  Expr *convertToRValueOld(Expr *E);
  Expr *convertToMaterializableOld(Expr *E);

  /// \brief Coerce the given expression to an rvalue, if it isn't already.
  Expr *coerceToRValue(Expr *expr);
  
  /// \brief Coerce the given expression to materializable type, if it
  /// isn't already.
  Expr *coerceToMaterializable(Expr *expr);

  /// conformsToProtocol - Determine whether the given type conforms to the
  /// given protocol.
  ///
  /// \param Conformance If non-NULL, and the type does conform to the given
  /// protocol, this will be set to the protocol conformance mapping that
  /// maps the given type \c T to the protocol \c Proto. The mapping may be
  /// NULL, if the mapping is trivial due to T being either an archetype or
  /// an existential type that directly implies conformance to \c Proto.
  ///
  /// \param ComplainLoc If valid, then this function will emit diagnostics if
  /// T does not conform to the given protocol. The primary diagnostic will
  /// be placed at this location, with notes for each of the protocol
  /// requirements not satisfied.
  ///
  /// \returns true if T conforms to the protocol Proto, false otherwise.
  bool conformsToProtocol(Type T, ProtocolDecl *Proto,
                          ProtocolConformance **Conformance = 0,
                          SourceLoc ComplainLoc = SourceLoc());

  /// \brief Given a set of archetype substitutions, verify and recird all of
  /// the required protocol-conformance relationships.
  bool checkSubstitutions(TypeSubstitutionMap &Substitutions,
                          ConformanceMap &Conformance,
                          SourceLoc ComplainLoc,
                          TypeSubstitutionMap *RecordSubstitutions = nullptr);

  /// \brief Determine whether the given type is compatible with an integer
  /// or floating-point literal and what function would perform the conversion.
  ///
  /// \returns The function that will perform the conversion, along with the
  /// type of the argument of this function.
  std::pair<FuncDecl*, Type> isLiteralCompatibleType(
                               Type Ty, SourceLoc Loc,
                               LiteralKind LitTy,
                               bool Complain,
                               bool RequiresBuiltinArg = false);

  /// \brief Lookup a member in the given type.
  ///
  /// \param type The type in which we will look for a member.
  /// \param name The name of the member to look for.
  /// \param members Will be populated with the set of members found.
  ///
  /// \returns true if any members were found, false otherwise.
  bool lookupMember(Type type, Identifier name,
                    SmallVectorImpl<ValueDecl *> &members);

  /// \brief Lookup the constructors of the given type.
  ///
  /// \param type The type for which we will look for constructors.
  /// \param constructors Will be populated with the set of constructors found.
  ///
  /// \returns true if any members were found, false otherwise.
  bool lookupConstructors(Type type,SmallVectorImpl<ValueDecl *> &constructors);

  /// @}

  /// \name Overload resolution
  ///
  /// Routines that perform overload resolution or provide diagnostics related
  /// to overload resolution.
  /// @{
  
  /// diagnoseEmptyOverloadSet - Diagnose a case where we disproved all of the
  /// possible candidates in an overload set of a call.
  void diagnoseEmptyOverloadSet(Expr *E, ArrayRef<ValueDecl *> Candidates);
  void printOverloadSetCandidates(ArrayRef<ValueDecl *> Candidates);

  /// \brief "Open" the archetypes of a type to make them deducible.
  ///
  /// \param T The type to be opened.
  ///
  /// \param GenericParams The generic parameter list, which contains the
  /// archetypes to be opened up (made deducible).
  ///
  /// \param CC The coercion context into which the types will be opened.
  /// After having opened the types, the coercion context contains the mappings
  /// needed to determine when all types have been deduced.
  ///
  /// \param OnlyInnermostParams Whether we should only up the innermost
  /// generic parameters. Otherwise, parameters from all levels will be opened.
  ///
  /// \returns The type T, with each of the archetypes in \c GenericParams
  /// substitued for deducible types.
  Type openPolymorphicType(Type T, const GenericParamList &GenericParams,
                           CoercionContext &CC, bool OnlyInnermostParams);

  /// \brief "Open" the archetypes of a set of types to make them deducible.
  ///
  /// \param Types The type to be opened, which will be mutated in place.
  ///
  /// \param GenericParams The generic parameter list, which contains the
  /// archetypes to be opened up (made deducible).
  ///
  /// \param CC The coercion context into which the types will be opened.
  /// After having opened the types, the coercion context contains the mappings
  /// needed to determine when all types have been deduced.
  ///
  /// \param OnlyInnermostParams Whether we should only up the innermost
  /// generic parameters. Otherwise, parameters from all levels will be opened.
  void openPolymorphicTypes(MutableArrayRef<Type> Types,
                            const GenericParamList &GenericParams,
                            CoercionContext &CC, bool OnlyInnermostParams);

  /// \brief Substitute a specific base type into the type T of a generic type
  /// member, returning the appropriately substituted type along with a coercion
  /// context.
  ///
  /// \param VD The declaration we're referring to.
  /// \param BaseTy The type of the base of the member reference.
  /// \param T The type of the declaration.
  /// \param Loc The location of this substitution.
  /// \param CC A fresh coercion context that will be populated with the
  /// substitutions.
  /// \param [out] GenericParams If non-NULL, will be provided with the generic
  /// parameter list used for substitution.
  Type substBaseForGenericTypeMember(ValueDecl *VD, Type BaseTy, Type T,
                                     SourceLoc Loc, CoercionContext &CC,
                                     GenericParamList **GenericParams =nullptr);

  /// \brief Substitute a specific base type into the type T of a generic type
  /// member, returning the appropriately substituted type along with a coercion
  /// context.
  ///
  /// \param VD The declaration we're referring to.
  /// \param BaseTy The type of the base of the member reference.
  /// \param Types An array of types that we'll substitute into.
  /// \param Loc The location of this substitution.
  /// \param CC A fresh coercion context that will be populated with the
  /// substitutions.
  /// \param [out] GenericParams If non-NULL, will be provided with the generic
  /// parameter list used for substitution.
  bool substBaseForGenericTypeMember(ValueDecl *VD, Type BaseTy,
                                     MutableArrayRef<Type> Types,
                                     SourceLoc Loc, CoercionContext &CC,
                                     GenericParamList **GenericParams =nullptr);

  /// checkPolymorphicApply - Check the application of a function of the given
  /// polymorphic type to a particular argument with, optionally, a destination
  /// type.
  OverloadCandidate checkPolymorphicApply(PolymorphicFunctionType *PolyFn,
                                          CoercionKind Kind, Expr *Arg,
                                          Type DestTy);

  /// checkPolymorphicUse - Check the use of a polymorphic function in the
  /// context of the given destination type.
  OverloadCandidate checkPolymorphicUse(PolymorphicFunctionType *PolyFn,
                                        Type DestTy, SourceLoc Loc,
                                        CoercionContext *CC);

  /// filterOverloadSet - Filter a set of overload candidates based on the
  /// the given argument type (for a call) or result type (if the context 
  /// provides such a type). 
  /// 
  /// This routine is used in both type-checking directions (root-to-leaf and
  /// leaf-to-root). The call argument allows leaf-to-root type checking
  /// to filter out candidates that can't be called as functions or have
  /// incompatible arguments. Conversely, when a destination type is provided,
  /// root-to-leaf type checking filters out candidates that cannot produce a
  /// result of an acceptable type. Both argument and destination type may be
  /// provided, which provides overload resolution based on both the call
  /// arguments and the expected result of the call.
  ///
  /// \param Candidates The set of overloaded candidates that should be 
  /// considered
  ///
  /// \param OperatorSyntax Whether this the overloaded function is being
  /// applied with operator syntax (e.g., a + b) rather than call syntax
  /// (e.g., +(a, b)).
  ///
  /// \param BaseTy The type of the object that will become the 'this' pointer
  /// for a call to a method. If not provided, then there is no 'this' object.
  ///
  /// \param Arg The call argument, to be passed to the function that is
  /// eventually selected by overload resolution. If null, we're performing
  /// overloading without a call.
  ///
  /// \param DestTy The type to which the result should be coerced, or null if
  /// not known.
  ///
  /// \param Viable If there is no single, complete vialbe candidate, output
  /// vector to which all of the viable candidates will be added.
  ///
  /// \returns The best candidate, if there is one.
  OverloadCandidate filterOverloadSet(ArrayRef<ValueDecl *> Candidates,
                                      bool OperatorSyntax,
                                      Type BaseTy,
                                      Expr *Arg,
                                      Type DestTy,
                                      SmallVectorImpl<ValueDecl *> &Viable);

  /// filterOverloadSetForValue - Filter a set of overload candidates based on
  /// a destination type to which the overload set is being coerced.
  ///
  /// \param Candidates The set of overloaded candidates that should be
  /// considered
  ///
  /// \param BaseTy The type of the object that will become the 'this' pointer
  /// for a call to a method. If not provided, then there is no 'this' object.
  ///
  /// \param DestTy The type to which the result should be coerced, or null if
  /// not known.
  ///
  /// \param Viable If there is no single, complete vialbe candidate, output
  /// vector to which all of the viable candidates will be added.
  ///
  /// \returns The best candidate, if there is one.
  OverloadCandidate filterOverloadSetForValue(ArrayRef<ValueDecl *> Candidates,
                                              SourceLoc Loc,
                                              Type BaseTy,
                                              Type DestTy,
                                          SmallVectorImpl<ValueDecl *> &Viable,
                                              CoercionContext *CC = nullptr);

  /// \brief Determine whether the given expression refer to an overloaded
  /// set of declarations and, if so, determine the candidates, base type,
  /// etc. required for overload resolution to proceed.
  OverloadedExpr getOverloadedExpr(Expr *E);

  /// buildFilteredOverloadSet - Given an overload set that has already been
  /// filtered, produce a new overload set with just the given set of
  /// declarations in it.
  Expr *buildFilteredOverloadSet(OverloadedExpr Ovl,
                                 ArrayRef<ValueDecl *> Remaining);

  /// buildFilteredOverloadSet - Given an overload set for which we have
  /// chosen a best candidate, return an expression that refers to that
  /// candidate.
  Expr *buildFilteredOverloadSet(OverloadedExpr Ovl,
                                 const OverloadCandidate &Candidate);

  /// \brief Encode the provided substitutions in the form used by
  /// SpecializeExpr (and another other AST nodes that require specialization).
  ///
  /// \param GenericParams The generic parameters whose substitutions are
  /// being encoded.
  ///
  /// \param Substitutions The set of substitutions.
  ///
  /// \param Conformances The set of protocol conformances.
  ///
  /// \param ArchetypesAreOpen Whether the substitutions refer to the opened
  /// form of the archetypes, as is used by the coercion code.
  ///
  /// \returns an ASTContext-allocate array of substitutions.
  ///
  /// \param OnlyInnermostParams Whether we're specializing only the innermost
  /// generic parameters (rather than all levels of generic parameters).
  ArrayRef<Substitution>
  encodeSubstitutions(const GenericParamList *GenericParams,
                      const TypeSubstitutionMap &Substitutions,
                      const ConformanceMap &Conformances,
                      bool ArchetypesAreOpen,
                      bool OnlyInnermostParams);

  /// \brief Build a new SpecializeExpr wrapping the given subexpression.
  ///
  /// \param Sub The subexpression to wrap, which must have polymorphic
  /// function type.
  ///
  /// \param Ty The type of the resulting SpecializeExpr.
  ///
  /// \param Substitutions The set of substitutions from each of the archetypes
  /// to the replacement type.
  ///
  /// \param Conformances The set of protocol-conformance structures for each
  /// of the substitutions.
  ///
  /// \param ArchetypesAreOpen Whether the substitutions refer to the opened
  /// form of the archetypes, as is used by the coercion code.
  ///
  /// \param OnlyInnermostParams Whether we're specializing only the innermost
  /// generic parameters (rather than all levels of generic parameters).
  SpecializeExpr *buildSpecializeExpr(Expr *Sub, Type Ty,
                                      const TypeSubstitutionMap &Substitutions,
                                      const ConformanceMap &Conformances,
                                      bool ArchetypesAreOpen,
                                      bool OnlyInnermostParams);

  /// \brief Build a reference to a declaration, where name lookup returned
  /// the given set of declarations.
  Expr *buildRefExpr(ArrayRef<ValueDecl *> Decls, SourceLoc NameLoc);

  /// \brief Build a reference to a (non-member) overload candidate.
  Expr *buildRefExpr(const OverloadCandidate &Candidate, SourceLoc NameLoc);

  /// \brief Specialize the given expression with substitutions from the
  /// given candidate.
  Expr *specializeOverloadResult(const OverloadCandidate &Candidate, Expr *E);

  /// \brief Build a reference to a member of the given base expression, where
  /// name lookup for the member returned the given set of declarations. 
  Expr *buildMemberRefExpr(Expr *Base, SourceLoc DotLoc,
                           ArrayRef<ValueDecl *> Decls,
                           SourceLoc MemberLoc);

  /// \brief Build a reference to a member of the given base expression,
  /// given the results of a successful member lookup.
  Expr *buildMemberRefExpr(Expr *Base, SourceLoc DotLoc,
                           MemberLookup &Results,
                           SourceLoc NameLoc);
  /// @}

  /// \brief Retrieve a specific, known protocol.
  ///
  /// \returns null if the protocol is not available. This represents a
  /// problem with the Standard Library.
  ProtocolDecl *getProtocol(KnownProtocolKind kind);

  /// \name AST Mutation Listener Implementation
  /// @{
  void handleExternalDecl(Decl *decl);

  /// \brief A new declaration was added to the AST.
  virtual void addedExternalDecl(Decl *decl);

  /// \brief A new type was added to the AST.
  virtual void addedExternalType(Type type);

  /// @}

};

} // end namespace swift

#endif
