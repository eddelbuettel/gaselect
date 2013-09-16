#ifndef RNG_H
#define RNG_H

#include "config.h"

class RNG {
	public:
		RNG(uint32_t seed);
//		~RNG() {}

		void seed(uint32_t seed);

		double operator()(double min, double range){
			return min + ((this->*genFun)() / RNG::RANDOM_MAX) * range;
		}
		uint32_t operator()() {
			return (this->*genFun)();
		}

	private:
		static const uint32_t W = 32;
		static const uint32_t R = 624;
		static const uint32_t DISCARD = 31;
		static const uint32_t MASKU = (0xffffffffU >> (RNG::W - RNG::DISCARD));
		static const uint32_t MASKL = (~RNG::MASKU);
		static const uint32_t M1 = 70;
		static const uint32_t M2 = 179;
		static const uint32_t M3 = 449;

		static const double RANDOM_MAX; // 2^W = 2^32

		int32_t stateIndex;
		uint32_t STATE[RNG::R];
		uint32_t z0;
		uint32_t z1;
		uint32_t z2;

		uint32_t (RNG::*genFun)(void);

		uint32_t case1(void); // stateIndex = 0
		uint32_t case2(void); // stateIndex = 1
		uint32_t case3(void); // stateIndex + M1 >= R
		uint32_t case4(void); // stateIndex + M3 >= R
		uint32_t case5(void); // stateIndex + M2 >= R
		uint32_t case6(void); // 2 <= stateIndex <= (R - M3 - 1)

	public:
		static const uint16_t RANDOM_BITS = RNG::W;
};

#endif
