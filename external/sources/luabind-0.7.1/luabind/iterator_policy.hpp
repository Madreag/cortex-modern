// Copyright Daniel Wallin 2007. Use, modification and distribution is
// subject to the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef LUABIND_ITERATOR_POLICY__071111_HPP
# define LUABIND_ITERATOR_POLICY__071111_HPP

# include <luabind/config.hpp>
# include <luabind/detail/policy.hpp>
# include <luabind/detail/convert_to_lua.hpp>
# include <luabind/detail/object_rep.hpp>
# include <iterator>
# include <utility>

namespace luabind { namespace detail {

template <class Container, bool Owned>
struct iterator
{
    typedef decltype(std::declval<Container&>().begin()) iterator_type;

    static int next(lua_State* L)
    {
        iterator* self = static_cast<iterator*>(lua_touserdata(L, lua_upvalueindex(1)));
        if (self->first != self->last)
        {
            convert_to_lua(L, *self->first);
            object_rep* value = is_class_object(L, -1);
            if (value && !(value->flags() & object_rep::owner) && !lua_isnil(L, lua_upvalueindex(2)))
                value->add_dependency(L, lua_upvalueindex(2));
            ++self->first;
        }
        else lua_pushnil(L);
        return 1;
    }

    static int destroy(lua_State* L)
    {
        static_cast<iterator*>(lua_touserdata(L, 1))->~iterator();
        return 0;
    }

    static int snapshot(lua_State* L)
    {
        iterator* self = static_cast<iterator*>(lua_touserdata(L, 1));
        lua_newtable(L);
        lua_pushboolean(L, Owned);
        lua_setfield(L, -2, "owned");
        lua_pushinteger(L, std::distance(self->container->begin(), self->first));
        lua_setfield(L, -2, "first");
        if (Owned)
        {
            lua_newtable(L);
            int index = 0;
            for (iterator_type item = self->first; item != self->last; ++item)
            {
                convert_to_lua(L, *item);
                object_rep* value = is_class_object(L, -1);
                if (value && !(value->flags() & object_rep::owner) && !lua_isnil(L, 2))
                    value->add_dependency(L, 2);
                lua_rawseti(L, -2, ++index);
            }
            lua_setfield(L, -2, "values");
            lua_pushinteger(L, index);
            lua_setfield(L, -2, "count");
        }
        else
        {
            lua_pushinteger(L, std::distance(self->container->begin(), self->last));
            lua_setfield(L, -2, "last");
        }
        return 1;
    }

    static int seek(lua_State* L)
    {
        iterator* self = static_cast<iterator*>(lua_touserdata(L, 1));
        lua_Integer first = lua_tointeger(L, 2), last = lua_tointeger(L, 3);
        bool valid = first >= 0 && last >= first && static_cast<size_t>(last) <= self->container->size();
        if (valid)
        {
            self->first = self->container->begin();
            std::advance(self->first, first);
            self->last = self->first;
            std::advance(self->last, last - first);
        }
        lua_pushboolean(L, valid);
        return 1;
    }

    explicit iterator(Container& source) : container(&source), first(source.begin()), last(source.end()) {}
    ~iterator() { if (Owned) delete container; }

    Container* container;
    iterator_type first, last;
};

template <bool Owned, class Container>
int make_range(lua_State* L, Container& container)
{
    typedef iterator<Container, Owned> range_type;
    const int argument_count = lua_gettop(L);
    void* storage = lua_newuserdata(L, sizeof(range_type));
    new (storage) range_type(container);
    static char metatable_key;
    lua_pushlightuserdata(L, &metatable_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushcfunction(L, range_type::destroy);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, range_type::snapshot);
        lua_setfield(L, -2, "__iterator_snapshot");
        lua_pushcfunction(L, range_type::seek);
        lua_setfield(L, -2, "__iterator_seek");
        lua_pushlightuserdata(L, &metatable_key);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_setmetatable(L, -2);
    if (is_class_object(L, 1)) lua_pushvalue(L, 1);
    else lua_pushnil(L);
    bool property = false;
    lua_Debug frame;
    if (!Owned && lua_getstack(L, 0, &frame) && lua_getinfo(L, "f", &frame))
    {
        lua_CFunction called = lua_tocfunction(L, -1);
        property = called == class_rep::gettable_dispatcher || called == class_rep::lua_class_gettable;
        if (property) { lua_pop(L, 1); lua_pushvalue(L, 2); }
    }
    else lua_pushnil(L);
    if (!Owned && !property && argument_count > 1)
    {
        lua_createtable(L, argument_count - 1, 0);
        for (int index = 2; index <= argument_count; ++index)
        {
            lua_pushvalue(L, index);
            lua_rawseti(L, -2, index - 1);
        }
    }
    else lua_pushnil(L);
    lua_pushcclosure(L, range_type::next, 4);
    return 1;
}

template <bool Owned, class Container>
int make_range(lua_State* L, Container* container)
{
    return make_range<Owned>(L, *container);
}

template <bool Owned>
struct iterator_converter
{
    typedef boost::mpl::bool_<false> is_value_converter;
    typedef iterator_converter type;

    template <class Container>
    void apply(lua_State* L, Container& container) { make_range<Owned>(L, container); }

    template <class Container>
    void apply(lua_State* L, Container const& container) { make_range<Owned>(L, container); }
};

template <bool Owned>
struct iterator_policy : conversion_policy<0>
{
    static void precall(lua_State*, index_map const&) {}
    static void postcall(lua_State*, index_map const&) {}

    template <class T, class Direction>
    struct apply { typedef iterator_converter<Owned> type; };
};

}} // namespace luabind::detail

namespace luabind { namespace {

LUABIND_ANONYMOUS_FIX detail::policy_cons<
    detail::iterator_policy<false>, detail::null_type> return_stl_iterator;

LUABIND_ANONYMOUS_FIX detail::policy_cons<
    detail::iterator_policy<true>, detail::null_type> return_stl_iterator_owned;

}} // namespace luabind::unnamed

#endif // LUABIND_ITERATOR_POLICY__071111_HPP
