#include "p2p_byte_stream_linux.h"
#include <unistd.h>

int P2PByteStreamLinux::Write(const void *buffer, int length) {
  int result = write(handler().fd, buffer, length);
  return result != -1 ? result : 0;
}

int P2PByteStreamLinux::Read(void *buffer, int length) {
  int result = read(handler().fd, buffer, length);
  return result != -1 ? result : 0;
}

int P2PByteStreamLinux::GetBurstMaxLength() {
  return 42;
}

int P2PByteStreamLinux::GetBurstIngestionNanosecondsPerByte() {
	return 250000;
}

int P2PByteStreamLinux::GetAtomicSendMaxLength() {
  return 4;
}

