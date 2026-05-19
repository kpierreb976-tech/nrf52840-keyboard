#ifndef MODE_SWITCH_H_
#define MODE_SWITCH_H_

#include <stdint.h>

enum mode_switch_position {
	MODE_SWITCH_POS_UNKNOWN = 0,
	MODE_SWITCH_POS_USB,
	MODE_SWITCH_POS_BLE,
	MODE_SWITCH_POS_2G4,
};

int mode_switch_init(void);
int mode_switch_get_position(enum mode_switch_position *pos);

#endif /* MODE_SWITCH_H_ */
