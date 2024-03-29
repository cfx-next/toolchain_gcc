// { dg-do compile }
// { dg-options "--std=c++0x" }
struct B1 {};

struct B2 final {};

struct D1 : B1 {};

struct D2 : B2 {}; // { dg-error "cannot derive from 'final' base" }

template<class T> struct D3 : T {};

template<class T> struct D4 : T {}; // { dg-error "cannot derive from 'final' base" }

template <class T> struct B3 {};

template <class T> struct B4 final {};

template <class T> struct B5 final {};

struct undeclared<int> final { }; // { dg-error "not a class template" }

struct D5 : B3<D5> {};

struct D6 : B4<D6> {}; // { dg-error "cannot derive from 'final' base" }

struct B6 final final {}; // { dg-error "duplicate virt-specifier" }

struct B7 override {}; // { dg-error "cannot specify 'override' for a class" }

namespace N
{
  struct C;
}

struct N::C final{};

int main()
{
  D3<B1> d;
  D4<B2> d2;
  struct B2 final{}; // { dg-error "previous definition" }
  B2 final; // { dg-error "has a previous declaration|previously declared here" }
  B2 final2 = final;
  struct B2 {}; // { dg-error "redefinition" }
  struct B2 final; // { dg-error "redeclaration" }
  struct B2 override; // { dg-message "previously declared here" }
  struct B2 final {}; // { dg-error "redefinition" }
  struct B2 override {}; // { dg-error "cannot specify 'override' for a class" }
  B2 override{}; // { dg-error "redeclaration" }
  struct foo final {}; // { dg-error "previous definition" }
  struct foo final {}; // { dg-error "redefinition" }
  foo final; // { dg-error "conflicting declaration" }
}
