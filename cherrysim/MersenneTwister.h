////////////////////////////////////////////////////////////////////////////////
//MIT License
//
//Copyright(c) 2016 Jakob "Brotcrunsher" Schaal
//
//Permission is hereby granted, free of charge, to any person obtaining a copy
//of this software and associated documentation files(the "Software"), to deal
//in the Software without restriction, including without limitation the rights
//to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//copies of the Software, and to permit persons to whom the Software is
//furnished to do so, subject to the following conditions :
//
//The above copyright notice and this permission notice shall be included in all
//copies or substantial portions of the Software.
//
//THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
//AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//SOFTWARE.
#pragma once

#include <stdint.h>
#include <ctime>

class MersenneTwister
{
private:
	//Numbers are from the definition of mt19937
	static constexpr uint16_t N = 624;
	static constexpr int M = 397;
	static constexpr int R = 31;
	static constexpr unsigned int A = 0x9908B0DF;
	static constexpr int F = 1812433253;
	static constexpr int U = 11;
	static constexpr int S = 7;
	static constexpr unsigned int B = 0x9D2C5680;
	static constexpr int T = 15;
	static constexpr unsigned int C = 0xEFC60000;
	static constexpr int L = 18;

	static constexpr uint64_t MASK_LOWER = (1ull << R) - 1;
	static constexpr uint64_t MASK_UPPER = (1ull << R);

	uint16_t m_index;
	uint32_t m_mt[N];

	void twistIteration(uint32_t i);
	void twist();

public:
	MersenneTwister();
	explicit MersenneTwister(uint32_t seed);

	void setSeed(uint32_t seed);

	uint32_t nextU32();
	double nextDouble();

	uint32_t nextU32(uint32_t min, uint32_t max);
	double nextDouble(double min, double max);
	double nextNormal(double mean, double sigma);
};


