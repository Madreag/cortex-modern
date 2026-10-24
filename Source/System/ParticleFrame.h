#pragma once

#include "Constants.h"
#include "DeterministicMath.h"

#include <cmath>

namespace RTE {
	/// The sprite frame a spinning particle shows for its rotation angle: the angle's place in its turn. Rounding can
	/// put an angle a hair below a whole turn just under zero, the end of the previous turn: its last frame.
	inline unsigned int ParticleTurnFrame(float angle, unsigned int frameCount) {
		double position = angle;
		position -= std::floor(angle / (2.0F * c_PI)) * (2.0F * c_PI);
		position /= (2.0F * c_PI);
		position *= frameCount;
		const unsigned int frame = DeterministicDoubleToUInt32(std::floor(position));
		return frame >= frameCount ? frameCount - 1 : frame;
	}
}
