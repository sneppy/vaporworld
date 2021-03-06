#pragma once

#include "core_types.h"
#include "const_ref.h"

/**
 * @class Compare templates/functional.h
 * 
 * Three-way comparison operator
 */
struct Compare
{
	/**
	 * Compares two operands.
	 * Requires operator< and operator> to be defined for both types
	 * 
	 * @param [in] a,b operands to compare
	 * @return 1 if a > b, -1 if a < b, 0 otherwise
	 */
	template<typename A, typename B>
	FORCE_INLINE int32 operator()(const A & a, const B & b) const
	{
		return int32(a > b) - int32(a < b);
	}
};