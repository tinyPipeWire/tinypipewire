/* SPDX-License-Identifier: MIT */

/**
 * @file tpw_export.h
 * @brief Marks the functions that make up the shared library's public ABI.
 */

#ifndef TPW_EXPORT_H
#define TPW_EXPORT_H

/* The library is built with hidden visibility, so only declarations carrying
 * TPW_API are exported. Defining TPW_API first overrides it. */
#ifndef TPW_API
#if defined(__GNUC__) && __GNUC__ >= 4
#define TPW_API __attribute__((visibility("default")))
#else
#define TPW_API
#endif
#endif

#endif /* TPW_EXPORT_H */
