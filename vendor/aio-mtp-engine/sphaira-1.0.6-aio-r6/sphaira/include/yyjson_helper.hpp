#pragma once

#include "defines.hpp"
#include <yyjson.h>
#include <string>
#include <type_traits>

constexpr auto cexprHash(const char *str, std::size_t v = 0) noexcept -> std::size_t {
    return (*str == 0) ? v : 31 * cexprHash(str + 1) + *str;
}

#define JSON_CHECK_TYPE(name, str) \
    static_assert( \
        (std::is_same<decltype(str), decltype(e.name)>()) || \
        (std::is_same<decltype(str), const char*>() && \
        (std::is_same<std::string, decltype(e.name)>())), \
        "json type missmatch" \
    )

#define JSON_CHECK_TYPE_ARR(name, str) \
    static_assert( \
        (std::is_same<decltype(str), decltype(e.name)::value_type>()) || \
        (std::is_same<decltype(str), const char*>() && \
        (std::is_same<std::string, decltype(e.name)::value_type>())), \
        "json type missmatch" \
    )

#define JSON_SKIP_IF_NULL_PTR(str) \
    if constexpr(std::is_pointer<decltype(str)>()) { \
        if (!str) { \
            continue; \
        } \
    }

#define JSON_SET_TYPE(name, type) case cexprHash(#name): { \
    if (yyjson_is_##type(val)) { \
        auto str = yyjson_get_##type(val); \
        JSON_CHECK_TYPE(name, str); \
        JSON_SKIP_IF_NULL_PTR(str); \
        e.name = str; \
    } \
}   break

#define JSON_SET_OBJ(name) case cexprHash(#name): { \
    if (yyjson_is_obj(val)) { \
        from_json(val, e.name); \
    } \
} break

#define JSON_SET_UINT(name) JSON_SET_TYPE(name, uint)
#define JSON_SET_STR(name) JSON_SET_TYPE(name, str)
#define JSON_SET_BOOL(name) JSON_SET_TYPE(name, bool)

#define JSON_SET_ARR_TYPE(name, type) { \
    if (yyjson_is_##type(val)) { \
        auto str = yyjson_get_##type(val); \
        JSON_CHECK_TYPE_ARR(name, str); \
        JSON_SKIP_IF_NULL_PTR(str); \
        e.name.emplace_back(str); \
    } \
}

#define JSON_SET_ARR(name, type) case cexprHash(#name): { \
    if (yyjson_is_arr(val)) { \
        const auto arr_size = yyjson_arr_size(val); \
        if (!arr_size) { \
            continue; \
        } \
        e.name.reserve(arr_size); \
        size_t idx, max; \
        yyjson_val *hit; \
        yyjson_arr_foreach(val, idx, max, hit) { \
            JSON_SET_ARR_TYPE(name, type); \
        } \
    } \
} break

#define JSON_SET_ARR_OBJ2(name, member) case cexprHash(#name): { \
    if (yyjson_is_arr(val)) { \
        const auto arr_size = yyjson_arr_size(val); \
        if (!arr_size) { \
            continue; \
        } \
        member.resize(arr_size); \
        size_t idx, max; \
        yyjson_val *hit; \
        yyjson_arr_foreach(val, idx, max, hit) { \
            from_json(hit, member[idx]); \
        } \
    } \
} break

#define JSON_SET_ARR_OBJ(name) JSON_SET_ARR_OBJ2(name, e.name)

#define JSON_SET_ARR_STR(name) JSON_SET_ARR_TYPE(name, str)

#define JSON_INIT_VEC_INTERNAL(obj) \
    if (!doc) { \
        return; \
    } \
    ON_SCOPE_EXIT(yyjson_doc_free(doc)); \
    const auto root = yyjson_doc_get_root(doc); \
    if (!root) { \
        return; \
    } \
    auto json = obj ? yyjson_obj_get(root, obj) : root; \
    if (!json) { \
        return; \
    }

#define JSON_INIT_VEC_FILE(path, obj, err) \
    auto doc = yyjson_read_file(path, YYJSON_READ_NOFLAG, nullptr, err); \
    JSON_INIT_VEC_INTERNAL(obj);

#define JSON_INIT_VEC(data, obj) \
    auto doc = yyjson_read((const char*)data.data(), data.size(), YYJSON_READ_NOFLAG); \
    JSON_INIT_VEC_INTERNAL(obj);

#define JSON_GET_OBJ(name) \
    json = yyjson_obj_get(json, name); \
    if (!json) { \
        return; \
    }

#define JSON_OBJ_ITR(...) \
    if (!yyjson_is_obj(json)) { \
        return; \
    } \
    yyjson_obj_iter iter; \
    yyjson_obj_iter_init(json, &iter); \
    yyjson_val *key; \
    while ((key = yyjson_obj_iter_next(&iter))) { \
        auto val = yyjson_obj_iter_get_val(key); \
        if (!val) { \
            continue; \
        } \
        const auto key_str = yyjson_get_str(key); \
        if (!key_str) { \
            continue; \
        } \
        switch (cexprHash(key_str)) { \
            __VA_ARGS__ \
        } \
    }

#define JSON_ARR_ITR(member) \
    if (!yyjson_is_arr(json)) { \
        return; \
    } \
    const auto arr_size = yyjson_arr_size(json); \
    if (!arr_size) { \
        return; \
    } \
    member.resize(arr_size); \
    size_t idx, max; \
    yyjson_val *hit; \
    yyjson_arr_foreach(json, idx, max, hit) { \
        from_json(hit, member[idx]); \
    }
