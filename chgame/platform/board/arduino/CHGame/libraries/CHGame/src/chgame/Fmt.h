/// @file Fmt.h
/// @brief Number formatting without printf.
///
/// The SDK's snprintf pulls in ~3.5 KB (and has no %l).
#pragma once
#include <stdint.h>

/// @defgroup chgame_fmt Number formatting
/// @ingroup lib_chgame
/// @brief Integers, money and times as text, without printf.
///
/// Each call writes at p, terminates the string and returns the new end, so
/// calls chain:
/// @code
/// char buf[16], *p = buf;
/// p = fmtStr(p, "BET "); p = fmtMoney(p, bet);    // "BET $25"
/// @endcode
/// The caller's buffer must be big enough: 12 characters hold any int32_t.
/// @{

/// @brief Write an integer: "-123".
/// @param p Where to write.
/// @param v The number.
/// @return The end of the string (at its terminating 0).
char *fmtInt(char *p, int32_t v);           // "-123"
/// @brief Write an amount of money: "$1234", "-$5".
/// @param p Where to write.
/// @param v Dollars.
/// @return The end of the string.
char *fmtMoney(char *p, int32_t v);         // "$1234", "-$5"
/// @brief Write money with thousands separators: "$1,234".
/// @param p Where to write.
/// @param v Dollars.
/// @return The end of the string.
char *fmtCash(char *p, int32_t v);          // "$1,234": money with thousands separators
/// @brief Write money short for a narrow space: as fmtMoney(), but "$12K" from $10,000.
/// @details From 10,000 on (either sign) it writes whole thousands, rounded
/// toward zero: 12,999 is "$12K".
/// @param p Where to write.
/// @param v Dollars.
/// @return The end of the string.
char *fmtShort(char *p, int32_t v);         // as fmtMoney, but "$12K" from $10,000
/// @brief Write a time as minutes and seconds: "1:05".
/// @param p    Where to write.
/// @param secs Seconds.
/// @return The end of the string.
char *fmtTime(char *p, uint16_t secs);      // "1:05"
/// @brief Copy a string.
/// @param p Where to write.
/// @param s The string.
/// @return The end of the copy.
char *fmtStr(char *p, const char *s);       // a copy of s

/// @}
