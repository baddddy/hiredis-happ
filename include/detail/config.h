//
// Created by ŷ��� on 2015/08/18.
//

#ifndef HIREDIS_HAPP_HIREDIS_HAPP_CONFIG_H
#define HIREDIS_HAPP_HIREDIS_HAPP_CONFIG_H

#pragma once

#include <stdint.h>
#include <cstddef>
#include <string>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <algorithm>


#if defined(__cplusplus)
extern "C" {
#endif

#include "hiredis.h"
#include "async.h"

#if defined(__cplusplus)
}
#endif


#define HIREDIS_HAPP_SLOT_NUMBER 16384

#if (defined(__cplusplus) && __cplusplus >= 201103L) || (defined(_MSC_VER) && _MSC_VER >= 1600)
#include <unordered_map> 
#define HIREDIS_HAPP_MAP(...) std::unordered_map< __VA_ARGS__ > 

#else
#include <map>
#define HIREDIS_HAPP_MAP(...) std::map< __VA_ARGS__ > 
#endif


#ifndef  HIREDIS_HAPP_TTL
#define HIREDIS_HAPP_TTL 16
#endif

#ifndef  HIREDIS_HAPP_TIMER_INTERVAL_SEC
// 0 s
#define HIREDIS_HAPP_TIMER_INTERVAL_SEC 0
#endif

#ifndef  HIREDIS_HAPP_TIMER_INTERVAL_USEC
// 100 ms
#define HIREDIS_HAPP_TIMER_INTERVAL_USEC 100000
#endif

#ifdef _MSC_VER
#define HIREDIS_HAPP_STRCASE_CMP(l, r) _stricmp(l, r)
#define HIREDIS_HAPP_STRNCASE_CMP(l, r, s) _strnicmp(l, r, s)
#define HIREDIS_HAPP_SSCANF(...) sscanf_s(__VA_ARGS__)

#else
#define HIREDIS_HAPP_STRCASE_CMP(l, r) strcasecmp(l, r)
#define HIREDIS_HAPP_STRNCASE_CMP(l, r, s) strncasecmp(l, r, s)
#define HIREDIS_HAPP_SSCANF(...) sscanf(__VA_ARGS__)

#endif

#ifdef _MSC_VER

#ifndef inline
#define inline __inline
#endif

#ifndef va_copy
#define va_copy(d,s) ((d) = (s))
#endif

#ifndef snprintf
#define snprintf c99_snprintf
#define vsnprintf c99_vsnprintf

__inline int c99_vsnprintf(char* str, size_t size, const char* format, va_list ap) {
    int count = -1;

    if (size != 0)
        count = _vsnprintf_s(str, size, _TRUNCATE, format, ap);
    if (count == -1)
        count = _vscprintf(format, ap);

    return count;
}

__inline int c99_snprintf(char* str, size_t size, const char* format, ...) {
    int count;
    va_list ap;

    va_start(ap, format);
    count = c99_vsnprintf(str, size, format, ap);
    va_end(ap);

    return count;
}
#endif

#endif


// error code
namespace hiredis {
    namespace happ {
#if (defined(__cplusplus) && __cplusplus >= 201103L) || (defined(_MSC_VER) && _MSC_VER >= 1600)
        template<typename T>
        struct unique_ptr {
            typedef std::unique_ptr<T> type;
            static void swap(type& l, type& r) {
                l.swap(r);
            }
        };
#else
        template<typename T>
        struct unique_ptr {
            typedef std::shared_ptr<T> type;
            static void swap(type& l, type& r) {
                using std::swap;
                swap(l, r);
            }
        };
#endif

        struct error_code {
            typedef enum {
                REDIS_HAPP_OK               =  REDIS_OK,
                REDIS_HAPP_UNKNOWD          = -1001, // unknown error
                REDIS_HAPP_HIREDIS          = -1002, // error happened in hiredis
                REDIS_HAPP_TTL              = -1003, // try more than ttl times
                REDIS_HAPP_CONNECTION       = -1004, // connect lost or connect failed
                REDIS_HAPP_SLOT_UNAVAILABLE = -1005, // slot unavailable
                REDIS_HAPP_CREATE           = -1006, // create failed
                REDIS_HAPP_PARAM            = -1007, // param error
                REDIS_HAPP_TIMEOUT          = -1008, // param error
            } type;
        };
    }
}

#endif //HIREDIS_HAPP_HIREDIS_HAPP_CONFIG_H
