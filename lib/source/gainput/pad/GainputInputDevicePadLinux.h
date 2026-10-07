
#ifndef GAINPUTINPUTDEVICEPADLINUX_H_
#define GAINPUTINPUTDEVICEPADLINUX_H_

#include <fcntl.h>
#include <unistd.h>
#include <linux/joystick.h>
#include <errno.h>

// Cf. http://www.kernel.org/doc/Documentation/input/joystick-api.txt
// Cf. http://ps3.jim.sh/sixaxis/usb/


namespace gainput
{

/// Maximum negative and positive value for an axis.
const float MaxAxisValue = 32767.0f;

static const char* PadDeviceIds[MaxPadCount] =
{
	"/dev/input/js0",
	"/dev/input/js1",
	"/dev/input/js2",
	"/dev/input/js3",
	"/dev/input/js4",
	"/dev/input/js5",
	"/dev/input/js6",
	"/dev/input/js7",
	"/dev/input/js8",
	"/dev/input/js9"
};

class InputDevicePadImplLinux : public InputDevicePadImpl
{
public:
	InputDevicePadImplLinux(InputManager& manager, InputDevice& device, unsigned index, InputState& state, InputState& previousState) :
		manager_(manager),
		device_(device),
		state_(state),
		index_(index),
		deviceState_(InputDevice::DS_UNAVAILABLE),
		fd_(-1),
		buttonDialect_(manager_.GetAllocator())
	{
		GAINPUT_ASSERT(index_ < MaxPadCount);
		CheckForDevice();
	}

	~InputDevicePadImplLinux()
	{
		if (fd_ != -1)
		{
			close(fd_);
		}
	}

	InputDevice::DeviceVariant GetVariant() const
	{
		return InputDevice::DV_STANDARD;
	}

	void Update(InputDeltaState* delta)
	{
		CheckForDevice();

		if (fd_ < 0)
		{
			return;
		}

		js_event event;
		int c;

		while ( (c = read(fd_, &event, sizeof(js_event))) == sizeof(js_event) )
		{
			event.type &= ~JS_EVENT_INIT;
			if (event.type == JS_EVENT_AXIS)
			{
				if (!axisDialect_.count(event.number))
				{
					continue;
				}
				const DeviceButtonId buttonId = axisDialect_[event.number];

				const float value = float(event.value)/MaxAxisValue;

				if (buttonId == PadButtonUp)
				{
					HandleButton(device_, state_, delta, PadButtonUp, value < 0.0f);
					HandleButton(device_, state_, delta, PadButtonDown, value > 0.0f);
				}
				else if (buttonId == PadButtonLeft)
				{
					HandleButton(device_, state_, delta, PadButtonLeft, value < 0.0f);
					HandleButton(device_, state_, delta, PadButtonRight, value > 0.0f);
				}
				else
				{
					HandleAxis(device_, state_, delta, buttonId, value);
				}
			}
			else if (event.type == JS_EVENT_BUTTON)
			{
				if (buttonDialect_.count(event.number))
				{
					DeviceButtonId buttonId = buttonDialect_[event.number];
					const bool value(event.value);

					HandleButton(device_, state_, delta, buttonId, value);
				}
#ifdef GAINPUT_DEBUG
				else
				{
					GAINPUT_LOG("Unknown pad button #%d: %d\n", int(event.number), event.value);
				}
#endif
			}
		}
		GAINPUT_ASSERT(c == -1);

		if (c == -1 
			&& (errno == EBADF || errno == ECONNRESET || errno == ENOTCONN || errno == EIO || errno == ENXIO || errno == ENODEV))
		{
#ifdef GAINPUT_DEBUG
			GAINPUT_LOG("Pad lost.\n");
#endif
			ClearState(delta);
			close(fd_);
			deviceState_ = InputDevice::DS_UNAVAILABLE;
			fd_ = -1;
		}
	}

	InputDevice::DeviceState GetState() const
	{
		return deviceState_;
	}

	bool IsValidButton(DeviceButtonId deviceButton) const
	{
		if (buttonDialect_.empty())
		{
			return deviceButton < PadButtonMax_;
		}

		for (HashMap<unsigned, DeviceButtonId>::const_iterator it = buttonDialect_.begin();
				it != buttonDialect_.end();
				++it)
		{
			if (it->second == deviceButton)
			{
				return true;
			}
		}

		for (HashMap<unsigned, DeviceButtonId>::const_iterator it = axisDialect_.begin();
				it != axisDialect_.end();
				++it)
		{
			if (it->second == deviceButton)
			{
				return true;
			}
		}

		return false;
	}

	bool Vibrate(float /*leftMotor*/, float /*rightMotor*/)
	{
		return false; // TODO
	}

private:
	InputManager& manager_;
	InputDevice& device_;
	InputState& state_;
	unsigned index_;
	InputDevice::DeviceState deviceState_;
	int fd_;
	HashMap<unsigned, DeviceButtonId> buttonDialect_;
	HashMap<unsigned, DeviceButtonId> axisDialect_;

	void ClearState(InputDeltaState* delta)
	{
		for (unsigned i = 0; i < PadButtonAxisCount_; ++i)
		{
			if (state_.GetFloat(i) != 0.0f)
			{
				HandleAxis(device_, state_, delta, i, 0.0f);
			}
		}
		for (unsigned i = PadButtonStart; i < PadButtonMax_; ++i)
		{
			if (state_.GetBool(i))
			{
				HandleButton(device_, state_, delta, i, false);
			}
		}
	}

	void MapKernelControls()
	{
		// Event numbers are positions in the driver's maps, not gamepad controls.
		unsigned char buttonCount = 0;
		__u16 buttonCodes[KEY_MAX - BTN_MISC + 1] = {};
		if (ioctl(fd_, JSIOCGBUTTONS, &buttonCount) >= 0
			&& ioctl(fd_, JSIOCGBTNMAP, buttonCodes) >= 0)
		{
			for (unsigned i = 0; i < buttonCount; ++i)
			{
				DeviceButtonId buttonId;
				switch (buttonCodes[i])
				{
				case BTN_SOUTH: buttonId = PadButtonA; break;
				case BTN_EAST: buttonId = PadButtonB; break;
				case BTN_WEST: buttonId = PadButtonX; break;
				case BTN_NORTH: buttonId = PadButtonY; break;
				case BTN_TL: buttonId = PadButtonL1; break;
				case BTN_TR: buttonId = PadButtonR1; break;
				case BTN_TL2: buttonId = PadButtonL2; break;
				case BTN_TR2: buttonId = PadButtonR2; break;
				case BTN_SELECT: buttonId = PadButtonSelect; break;
				case BTN_START: buttonId = PadButtonStart; break;
				case BTN_MODE: buttonId = PadButtonHome; break;
				case BTN_THUMBL: buttonId = PadButtonL3; break;
				case BTN_THUMBR: buttonId = PadButtonR3; break;
				case BTN_DPAD_LEFT: buttonId = PadButtonLeft; break;
				case BTN_DPAD_RIGHT: buttonId = PadButtonRight; break;
				case BTN_DPAD_UP: buttonId = PadButtonUp; break;
				case BTN_DPAD_DOWN: buttonId = PadButtonDown; break;
				default: continue;
				}
				buttonDialect_[i] = buttonId;
			}
		}

		unsigned char axisCount = 0;
		__u8 axisCodes[ABS_CNT] = {};
		if (ioctl(fd_, JSIOCGAXES, &axisCount) >= 0
			&& axisCount <= ABS_CNT
			&& ioctl(fd_, JSIOCGAXMAP, axisCodes) >= 0)
		{
			axisDialect_.clear();
			for (unsigned i = 0; i < axisCount; ++i)
			{
				DeviceButtonId buttonId;
				switch (axisCodes[i])
				{
				case ABS_X: buttonId = PadButtonLeftStickX; break;
				case ABS_Y: buttonId = PadButtonLeftStickY; break;
				case ABS_RX: buttonId = PadButtonRightStickX; break;
				case ABS_RY: buttonId = PadButtonRightStickY; break;
				case ABS_Z: buttonId = PadButtonAxis4; break;
				case ABS_RZ: buttonId = PadButtonAxis5; break;
				case ABS_HAT0X:
					buttonId = PadButtonLeft;
					axisDialect_[unsigned(-2)] = PadButtonRight;
					break;
				case ABS_HAT0Y:
					buttonId = PadButtonUp;
					axisDialect_[unsigned(-1)] = PadButtonDown;
					break;
				default: continue;
				}
				axisDialect_[i] = buttonId;
			}
		}
	}

	void CheckForDevice()
	{
		if (fd_ != -1)
		{
			return;
		}

		deviceState_ = InputDevice::DS_UNAVAILABLE;

		fd_ = open(PadDeviceIds[index_], O_RDONLY | O_NONBLOCK);
		if (fd_ < 0)
		{
			return;
		}
		GAINPUT_ASSERT(fd_ >= 0);

#ifdef GAINPUT_DEBUG
		char axesCount;
		ioctl(fd_, JSIOCGAXES, &axesCount);
		GAINPUT_LOG("Axes count: %d\n", int(axesCount));

		int driverVersion;
		ioctl(fd_, JSIOCGVERSION, &driverVersion);
		GAINPUT_LOG("Driver version: %d\n", driverVersion);
#endif

		char name[128] = "";
		if (ioctl(fd_, JSIOCGNAME(sizeof(name)), name) < 0)
		{
			strncpy(name, "Unknown", sizeof(name));
		}
#ifdef GAINPUT_DEBUG
		GAINPUT_LOG("Name: %s\n", name);
#endif

		buttonDialect_.clear();
		axisDialect_.clear();
		for (unsigned i = PadButtonLeftStickX; i < PadButtonAxisCount_; ++i)
		{
			axisDialect_[i] = i;
		}

		if (strcmp(name, "Sony Computer Entertainment Wireless Controller") == 0)
        {
            close(fd_);
            fd_ = -1;
            return;
        }
		else if (strcmp(name, "Sony PLAYSTATION(R)3 Controller") == 0)
		{
#ifdef GAINPUT_DEBUG
			GAINPUT_LOG("  --> known controller\n");
#endif
			buttonDialect_[0] = PadButtonSelect;
			buttonDialect_[1] = PadButtonL3;
			buttonDialect_[2] = PadButtonR3;
			buttonDialect_[3] = PadButtonStart;
			buttonDialect_[4] = PadButtonUp;
			buttonDialect_[5] = PadButtonRight;
			buttonDialect_[6] = PadButtonDown;
			buttonDialect_[7] = PadButtonLeft;
			buttonDialect_[8] = PadButtonL2;
			buttonDialect_[9] = PadButtonR2;
			buttonDialect_[10] = PadButtonL1;
			buttonDialect_[11] = PadButtonR1;
			buttonDialect_[12] = PadButtonY;
			buttonDialect_[13] = PadButtonB;
			buttonDialect_[14] = PadButtonA;
			buttonDialect_[15] = PadButtonX;
			buttonDialect_[16] = PadButtonHome;
		}
		else if (strcmp(name, "Microsoft X-Box 360 pad") == 0)
		{
#ifdef GAINPUT_DEBUG
			GAINPUT_LOG("  --> known controller\n");
#endif
			buttonDialect_[6] = PadButtonSelect;
			buttonDialect_[9] = PadButtonL3;
			buttonDialect_[10] = PadButtonR3;
			buttonDialect_[7] = PadButtonStart;
			buttonDialect_[4] = PadButtonL1;
			buttonDialect_[5] = PadButtonR1;
			buttonDialect_[3] = PadButtonY;
			buttonDialect_[1] = PadButtonB;
			buttonDialect_[0] = PadButtonA;
			buttonDialect_[2] = PadButtonX;
			buttonDialect_[8] = PadButtonHome;

			axisDialect_[3] = PadButtonRightStickX;
			axisDialect_[4] = PadButtonRightStickY;
			axisDialect_[2] = PadButtonAxis4;
			axisDialect_[5] = PadButtonAxis5;
			axisDialect_[7] = PadButtonUp;
			axisDialect_[6] = PadButtonLeft;

			// Dummy entries for IsValidButton
			axisDialect_[-1] = PadButtonDown;
			axisDialect_[-2] = PadButtonRight;
		}
		else
		{
			MapKernelControls();
		}

		deviceState_ = InputDevice::DS_OK;
	}
};

}

#endif

