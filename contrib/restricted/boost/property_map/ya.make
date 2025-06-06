# Generated by devtools/yamaker from nixpkgs 24.05.

LIBRARY()

LICENSE(BSL-1.0)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(1.87.0)

ORIGINAL_SOURCE(https://github.com/boostorg/property_map/archive/boost-1.87.0.tar.gz)

PEERDIR(
    contrib/restricted/boost/any
    contrib/restricted/boost/assert
    contrib/restricted/boost/concept_check
    contrib/restricted/boost/config
    contrib/restricted/boost/core
    contrib/restricted/boost/function
    contrib/restricted/boost/iterator
    contrib/restricted/boost/lexical_cast
    contrib/restricted/boost/mpl
    contrib/restricted/boost/smart_ptr
    contrib/restricted/boost/static_assert
    contrib/restricted/boost/throw_exception
    contrib/restricted/boost/type_traits
    contrib/restricted/boost/utility
)

ADDINCL(
    GLOBAL contrib/restricted/boost/property_map/include
)

NO_COMPILER_WARNINGS()

NO_UTIL()

END()
