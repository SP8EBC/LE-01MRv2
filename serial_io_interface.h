#ifndef C11D1360_4672_4779_82D2_1B8881C12248
#define C11D1360_4672_4779_82D2_1B8881C12248

#include <array>
#include <cstdint>

class SerialIoIf {
  public:
	virtual void requestRegisters (uint16_t slaveid, uint16_t startregister, uint8_t num,
								   std::array<uint16_t, 16> &out) = 0;
};

#endif /* C11D1360_4672_4779_82D2_1B8881C12248 */
