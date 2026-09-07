// Copyright Daniel Wallin 2007. Use, modification and distribution is
// subject to the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef LUABIND_ITERATOR_POLICY__071111_HPP
# define LUABIND_ITERATOR_POLICY__071111_HPP

# include <luabind/config.hpp>
# include <luabind/detail/policy.hpp>
# include <luabind/detail/convert_to_lua.hpp>
# include <luabind/detail/object_rep.hpp>

namespace luabind { namespace detail {

template <class Iterator>
struct iterator
{
    static int next(lua_State* L)
    {
        iterator* self = static_cast<iterator*>(
            lua_touserdata(L, lua_upvalueindex(1)));

        if (self->first != self->last)
        {
            convert_to_lua(L, *self->first);
            object_rep* value = is_class_object(L, -1);
            if (value && !(value->flags() & object_rep::owner) && !lua_isnil(L, lua_upvalueindex(2)))
                value->add_dependency(L, lua_upvalueindex(2));
            ++self->first;
        }
        else
        {
            lua_pushnil(L);
        }

        return 1;
    }

    static int destroy(lua_State* L)
    {
        iterator* self = static_cast<iterator*>(
            lua_touserdata(L, 1));
        self->~iterator();
        return 0;
    }

    iterator(Iterator first, Iterator last)
      : first(first)
      , last(last)
    {}

    Iterator first;
    Iterator last;
};

template <class Iterator>
int make_range(lua_State* L, Iterator first, Iterator last)
{
    void* storage = lua_newuserdata(L, sizeof(iterator<Iterator>));
    lua_newtable(L);
    lua_pushcclosure(L, iterator<Iterator>::destroy, 0);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    if (is_class_object(L, 1)) lua_pushvalue(L, 1);
    else lua_pushnil(L);
    lua_pushcclosure(L, iterator<Iterator>::next, 2);
    new (storage) iterator<Iterator>(first, last);
    return 1;
}

template <class Container>
int make_range(lua_State* L, Container& container)
{
    return make_range(L, container.begin(), container.end());
}

template <class Container>
int make_range(lua_State* L, Container* container)
{
    return make_range(L, container->begin(), container->end());
}

struct iterator_converter
{
    typedef boost::mpl::bool_<false> is_value_converter;
    typedef iterator_converter type;

    template <class Container>
    void apply(lua_State* L, Container& container)
    {
        make_range(L, container);
    }

    template <class Container>
    void apply(lua_State* L, Container const& container)
    {
        make_range(L, container);
    }
};

struct iterator_policy : conversion_policy<0>
{
    static void precall(lua_State*, index_map const&)
    {}

    static void postcall(lua_State*, index_map const&)
    {}

    template <class T, class Direction>
    struct apply
    {
        typedef iterator_converter type;
    };
};

}} // namespace luabind::detail

namespace luabind { namespace {

LUABIND_ANONYMOUS_FIX detail::policy_cons<
    detail::iterator_policy, detail::null_type> return_stl_iterator;

}} // namespace luabind::unnamed

#endif // LUABIND_ITERATOR_POLICY__071111_HPP
