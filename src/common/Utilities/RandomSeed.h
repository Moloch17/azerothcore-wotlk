/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef RandomSeed_h__
#define RandomSeed_h__

#include "Define.h"

/* Restart the calling thread's random sequence (urand, irand, frand, rand_norm, ...) from `seed`, so code run
 * right after it draws the same numbers every time. 0 reseeds from std::random_device. Other threads are not
 * affected. */
AC_COMMON_API void rand_seed(uint32 seed);

#endif // RandomSeed_h__
