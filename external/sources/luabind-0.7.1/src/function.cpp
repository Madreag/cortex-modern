// Copyright (c) 2003 Daniel Wallin and Arvid Norberg

// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
// ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
// OR OTHER DEALINGS IN THE SOFTWARE.

#include <luabind/lua_include.hpp>

#include <luabind/config.hpp>
#include <luabind/luabind.hpp>
#include <luabind/detail/object_rep.hpp>

#include <map>
#include <mutex>
#include <set>
#include <string>

namespace luabind { namespace detail { namespace free_functions {

    namespace
    {
        // Which Lua arguments of an overload the callee may write: a pointer or a reference to something non-const, read
        // once from the overload's signature. Bit n stands for the argument at stack index n.
        struct mutable_arguments
        {
            unsigned pointers = 0;
            unsigned references = 0;
        };

        mutable_arguments read_mutable_arguments(lua_State* L, overload_rep const& o)
        {
            static std::mutex mutex;
            static std::map<overload_rep const*, mutable_arguments> known;
            std::lock_guard<std::mutex> lock(mutex);
            std::map<overload_rep const*, mutable_arguments>::const_iterator found = known.find(&o);
            if (found != known.end()) return found->second;

            mutable_arguments arguments;
#ifndef LUABIND_NO_ERROR_CHECKING
            std::string signature;
            o.get_signature(L, signature);
            const std::string::size_type open = signature.find('(');
            const std::string::size_type close = signature.rfind(')');
            if (open != std::string::npos && close != std::string::npos && close > open)
            {
                int depth = 0;
                int index = 0;
                std::string parameter;
                for (std::string::size_type i = open + 1; i <= close; ++i)
                {
                    const char c = signature[i];
                    if (i == close || (c == ',' && depth == 0))
                    {
                        const std::string::size_type first = parameter.find_first_not_of(' ');
                        const std::string::size_type last = parameter.find_last_not_of(' ');
                        const std::string type = first == std::string::npos ? std::string() : parameter.substr(first, last - first + 1);
                        parameter.clear();
                        // A raw lua_State parameter takes no Lua argument.
                        if (type.empty() || type == "lua_State*") continue;
                        ++index;
                        const bool constant = type.compare(0, 6, "const ") == 0;
                        if (!constant && index < 32 && type[type.size() - 1] == '*') arguments.pointers |= 1U << index;
                        if (!constant && index < 32 && type[type.size() - 1] == '&') arguments.references |= 1U << index;
                        continue;
                    }
                    if (c == '<' || c == '[' || c == '(') ++depth;
                    else if (c == '>' || c == ']' || c == ')') --depth;
                    parameter += c;
                }
            }
#else
            (void)L;
#endif
            known[&o] = arguments;
            return arguments;
        }
    }

    void function_rep::add_overload(overload_rep const& o)
    {
        // A name built for the registration (To<Class>, Create<Class>) dies with its statement; the function keeps its own copy.
        static std::mutex mutex;
        static std::set<std::string> names;
        {
            std::lock_guard<std::mutex> lock(mutex);
            m_name = names.insert(m_name).first->c_str();
        }

        std::vector<overload_rep>::iterator i = std::find(
            m_overloads.begin(), m_overloads.end(), o);

        // if the overload already exists, overwrite the existing function
        if (i != m_overloads.end())
        {
            *i = o;
        }
        else
        {
            m_overloads.push_back(o);
        }
    }

    int function_dispatcher(lua_State* L)
    {
        function_rep* rep = static_cast<function_rep*>(
            lua_touserdata(L, lua_upvalueindex(1))
        );

        bool ambiguous = false;
        int min_match = std::numeric_limits<int>::max();
        int match_index = -1;
        bool ret;

#ifdef LUABIND_NO_ERROR_CHECKING
        if (rep->overloads().size() == 1)
        {
            match_index = 0;
        }
        else
        {
#endif
            int num_params = lua_gettop(L);
            ret = find_best_match(
                L
              , &rep->overloads().front()
              , (int)rep->overloads().size()
              , sizeof(overload_rep)
              , ambiguous
              , min_match
              , match_index
              , num_params
            );
#ifdef LUABIND_NO_ERROR_CHECKING
        }
#else
        if (!ret)
        {
            // this bock is needed to make sure the std::string is destructed
            {
                std::string msg = "no match for function call '";
                msg += rep->name();
                msg += "' with the parameters (";
                msg += stack_content_by_name(L, 1);
                msg += ")\ncandidates are:\n";

                msg += get_overload_signatures(
                    L
                  , rep->overloads().begin()
                  , rep->overloads().end()
                  , rep->name()
                );

                lua_pushstring(L, msg.c_str());
            }

            lua_error(L);
        }

        if (ambiguous)
        {
            // this bock is needed to make sure the std::string is destructed
            {
                std::string msg = "call of overloaded function '";
                msg += rep->name();
                msg += "(";
                msg += stack_content_by_name(L, 1);
                msg += ") is ambiguous\nnone of the overloads "
                       "have a best conversion:";

                std::vector<overload_rep_base const*> candidates;
                find_exact_match(
                    L
                  , &rep->overloads().front()
                  , (int)rep->overloads().size()
                  , sizeof(overload_rep)
                  , min_match
                  , num_params
                  , candidates
                );

                msg += get_overload_signatures_candidates(
                    L
                  , candidates.begin()
                  , candidates.end()
                  , rep->name()
                );

                lua_pushstring(L, msg.c_str());
            }
            lua_error(L);
        }
#endif
        overload_rep const& ov_rep = rep->overloads()[match_index];

        // Inside a preview window a free function takes its arguments by the rule a method does: the window decides what
        // an object handed to a parameter the callee may write means for the call, and a refused argument drops the call.
        if (preview_fence_window && preview_fence::argument)
        {
            const int top = lua_gettop(L);
            bool read = false;
            mutable_arguments arguments;
            for (int index = 1; index <= top; ++index)
            {
                if (!is_class_object(L, index)) continue;
                if (!read)
                {
                    arguments = read_mutable_arguments(L, ov_rep);
                    read = true;
                }
                const bool pointer = index < 32 && ((arguments.pointers >> index) & 1U) != 0;
                const bool reference = index < 32 && ((arguments.references >> index) & 1U) != 0;
                if (!preview_fence::argument(L, index, pointer, reference, 0, rep->name())) return 0;
            }
        }

#ifndef LUABIND_NO_EXCEPTIONS
        try
        {
#endif
            return ov_rep.call(L, ov_rep.fun);
#ifndef LUABIND_NO_EXCEPTIONS
        }
        catch(const luabind::error&)
        {
        }
        catch(const std::exception& e)
        {
            lua_pushstring(L, e.what());
        }
        catch (const char* s)
        {
            lua_pushstring(L, s);
        }
        catch(...)
        {
            std::string msg = rep->name();
            msg += "() threw an exception";
            lua_pushstring(L, msg.c_str());
        }
        // we can only reach this line if an exception was thrown
        lua_error(L);
        return 0; // will never be reached
#endif
    }

}}} // namespace luabind::detail::free_functions

