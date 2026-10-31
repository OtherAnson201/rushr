#pragma once
#include <windows.h>
#include <sdk/math/math.h>

namespace rbx
{
	struct obb final
	{
		math::vector3 center;
		math::vector3 half_size;
		math::vector3 axes[3];

		float radius;

		obb(const math::vector3& center, const math::vector3& size, const math::matrix3& rotation)
			: center(center), half_size(size * 0.5f), radius((size * 0.5f).length())
		{

			axes[0] = math::vector3(rotation.m[0][0], rotation.m[1][0], rotation.m[2][0]);
			axes[1] = math::vector3(rotation.m[0][1], rotation.m[1][1], rotation.m[2][1]);
			axes[2] = math::vector3(rotation.m[0][2], rotation.m[1][2], rotation.m[2][2]);
		}

		bool intersects(const math::vector3& origin, const math::vector3& dir, float max_distance, float& t_enter, float& t_exit) const
		{
			float tmin = 0.0f;
			float tmax = max_distance;

			math::vector3 p = center - origin;

			for (int i = 0; i < 3; ++i)
			{
				float e = axes[i].dot(p);
				float f = axes[i].dot(dir);

				const float EPSILON = 1e-6f;
				if (fabs(f) > EPSILON)
				{
					float t1 = (e + half_size[i]) / f;
					float t2 = (e - half_size[i]) / f;

					if (t1 > t2) std::swap(t1, t2);

					if (t1 > tmin) tmin = t1;
					if (t2 < tmax) tmax = t2;

					if (tmax < tmin)
					{
						return false;
					}
				}
				else
				{
					if (-e - half_size[i] > 0.0f || -e + half_size[i] < 0.0f)
					{
						return false;
					}
				}
			}

			t_enter = tmin;
			t_exit = tmax;
			return true;
		}

		bool blocks(const math::vector3& origin, const math::vector3& dir, float distance) const
		{

			const math::vector3 to_center = center - origin;
			const float along = to_center.dot(dir);
			if (along < -radius || along > distance + radius)
				return false;
			if (to_center.length_sq() - along * along > radius * radius)
				return false;

			float t_enter = 0.0f;
			float t_exit = 0.0f;
			if (!intersects(origin, dir, distance, t_enter, t_exit))
				return false;

			const float margin = 0.05f;
			return t_enter > margin && t_exit < distance - margin;
		}
	};
}
