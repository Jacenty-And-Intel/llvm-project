enum E { e1, e2, e3 };
enum class EC { e1, e2, e3 };

struct A {
  int i;
  long l;
  float f;
  double d;
  E e;
  EC ec;
};

struct Outer {
  int i;
  struct Inner {
    long l;
  };
};

extern constexpr A a{42, 47l, 4.2f, 4.7, e1, EC::e3};
extern constexpr E e(e2);
extern constexpr EC ec(EC::e2);
extern constexpr Outer outer{47};
extern constexpr Outer::Inner outer_inner{42l};
