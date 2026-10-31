#include <sdk/sdk.h>
#include <Offsets/Offsets.hpp>
#include <memory/memory.h>
#include <game/game.h>
#include <xmmintrin.h>
#include <cmath>

// the instances Name
std::string rbx::nameable_t::get_name() const
{

	std::uint64_t name_container = memory->read<std::uint64_t>(this->address + Offsets::Instance::NameContainer);

	if (name_container)
	{
		return memory->read_string(name_container + Offsets::Instance::Name);
	}

	return "unknown";
}

// the instances ClassName
std::string rbx::nameable_t::get_class_name() const
{
	std::uint64_t class_descriptor = memory->read<std::uint64_t>(this->address + Offsets::Instance::ClassDescriptor);
	std::uint64_t class_name = memory->read<std::uint64_t>(class_descriptor + Offsets::Instance::ClassName);

	if (class_name)
	{
		return memory->read_string(class_name);
	}

	return "unknown";
}

// the instances child objects
std::vector<rbx::instance_t> rbx::interface_t::get_children() const
{
	const rbx::instance_t* base = static_cast<const rbx::instance_t*>(this);
	std::vector<rbx::instance_t> children;

	if (base->address == 0)
		return children;

	try
	{
		std::uint64_t children_struct = memory->read<std::uint64_t>(base->address + Offsets::Instance::ChildrenStart);
		if (children_struct == 0)
			return children;

		std::uint64_t start = memory->read<std::uint64_t>(children_struct);
		std::uint64_t end = memory->read<std::uint64_t>(children_struct + Offsets::Instance::ChildrenEnd);

		if (start == 0 || end == 0 || end <= start)
			return children;

		constexpr std::uint64_t kStep = 16;
		constexpr std::uint64_t kMaxChildren = 10000;
		constexpr std::uint64_t kMaxByteSize = kMaxChildren * kStep;

		std::uint64_t byte_size = end - start;
		if (byte_size > kMaxByteSize)
			return children;

		if (byte_size % kStep != 0)
			return children;

		std::uint64_t count = byte_size / kStep;
		if (count == 0)
			return children;

		children.reserve(static_cast<size_t>(count));

		for (std::uint64_t instance = start; instance < end; instance += kStep)
		{
			std::uint64_t child_address = memory->read<std::uint64_t>(instance);
			if (child_address != 0 && child_address != 0xFFFFFFFFFFFFFFFF)
			{
				children.emplace_back(child_address);
			}
		}
	}
	catch (...)
	{
		return children;
	}

	return children;
}

// first child with a given name
rbx::instance_t rbx::interface_t::find_first_child(std::string_view str) const
{
	std::vector<rbx::instance_t> children = this->get_children();

	for (const rbx::instance_t& child : children)
	{
		if (child.get_name() == str)
		{
			return child;
		}
	}

	return {};
}

// first child of a given class
rbx::instance_t rbx::interface_t::find_first_child_by_class(std::string_view str) const
{
	std::vector<rbx::instance_t> children = this->get_children();

	for (const rbx::instance_t& child : children)
	{
		if (child.get_class_name() == str)
		{
			return child;
		}
	}

	return {};
}

// the players character model
rbx::model_instance_t rbx::player_t::get_model_instance()
{
	return { memory->read<std::uint64_t>(this->address + Offsets::Player::ModelInstance) };
}

// the players display name
std::string rbx::player_t::get_display_name()
{
	return memory->read_string(this->address + Offsets::Player::DisplayName);
}

// r6 or r15 rig type
std::uint8_t rbx::humanoid_t::get_rig_type()
{
	return { memory->read<std::uint8_t>(this->address + Offsets::Humanoid::RigType) };
}

// the humanoids state value
std::uint16_t rbx::humanoid_t::get_state()
{
	auto humanoid_state{ memory->read<std::uint64_t>(this->address + Offsets::Humanoid::HumanoidState) };
	return { memory->read<std::uint16_t>(humanoid_state + Offsets::Humanoid::HumanoidStateID) };
}

// the parts physics body
rbx::primitive_t rbx::part_t::get_primitive() const
{
	return { memory->read<std::uint64_t>(this->address + Offsets::BasePart::Primitive) };
}

// the parts size
math::vector3 rbx::primitive_t::get_size()
{
	return memory->read<math::vector3>(this->address + Offsets::Primitive::Size);
}

// the parts world position
math::vector3 rbx::primitive_t::get_position()
{
	return memory->read<math::vector3>(this->address + Offsets::Primitive::Position);
}

// the parts rotation matrix
math::matrix3 rbx::primitive_t::get_rotation()
{
	return memory->read<math::matrix3>(this->address + Offsets::Primitive::Rotation);
}

// the viewports size
math::vector2 rbx::visualengine_t::get_dimensions()
{
	return memory->read<math::vector2>(this->address + Offsets::VisualEngine::Dimensions);
}

// the cameras view matrix
math::matrix4 rbx::visualengine_t::get_viewmatrix()
{
	return memory->read<math::matrix4>(this->address + Offsets::VisualEngine::ViewMatrix);
}

// project a world point to the screen
bool rbx::visualengine_t::world_to_screen(const math::vector3& world, math::vector2& out, const math::vector2& dims, const math::matrix4& view)
{
	math::vector4 clip = view.multiply({ world.x, world.y, world.z, 1.f });

	if (clip.w < 0.1f)
	{
		return false;
	}

	clip.x /= clip.w;
	clip.y /= clip.w;

	out.x = (dims.x * 0.5f * clip.x) + (dims.x * 0.5f);
	out.y = -(dims.y * 0.5f * clip.y) + (dims.y * 0.5f);

	out.x += game::window_offset_x;
	out.y += game::window_offset_y;

	return true;
}

// set the cameras viewport size
void rbx::camera_t::set_viewport(const math::vector2int16& val)
{
	if (this->address == 0)
		return;

	memory->write<math::vector2int16>(this->address + Offsets::Camera::Viewport, val);
}

// compute the viewport size
math::vector2int16 rbx::camera_t::CalculateViewport(const math::vector2& TargetPosition, const math::vector2& Size, const math::vector2& MousePosition)
{
	math::vector2int16 Result;

	double TargetY = static_cast<double>(TargetPosition.y);
	if (TargetY < 1.0) TargetY = 1.0;
	if (TargetY > static_cast<double>(Size.y) - 1.0) TargetY = static_cast<double>(Size.y) - 1.0;

	double Ratio = static_cast<double>(MousePosition.y) / TargetY;
	double VY = static_cast<double>(Size.y) * Ratio;

	if (VY > 32767.0) VY = 32767.0;
	if (VY < 1.0) VY = 1.0;

	Ratio = VY / static_cast<double>(Size.y);
	double VX = 2.0 * static_cast<double>(MousePosition.x) - Ratio * (2.0 * static_cast<double>(TargetPosition.x) - static_cast<double>(Size.x));

	if (VX > 32767.0) VX = 32767.0;
	if (VX < 1.0) VX = 1.0;

	Result.x = static_cast<int16_t>(std::round(VX));
	Result.y = static_cast<int16_t>(std::round(VY));
	return Result;
}

std::uint64_t rbx::c_silent_help::cached_input_object = 0;

// the current input object address
static std::uint64_t get_current_input_object(std::uint64_t base_address)
{
	std::uint64_t object_address = memory->read<std::uint64_t>(base_address + Offsets::MouseService::InputObject + sizeof(std::shared_ptr<void*>));
	return object_address;
}

// write the silent-aim mouse position
void rbx::c_silent_help::write_mouse_position(std::uint64_t address, float x, float y)
{
	cached_input_object = get_current_input_object(address);
	if (cached_input_object != 0 && cached_input_object != 0xFFFFFFFFFFFFFFFF)
	{
		math::vector2 new_position = { x, y };
		memory->write<math::vector2>(cached_input_object + Offsets::MouseService::MousePosition, new_position);
	}
}
