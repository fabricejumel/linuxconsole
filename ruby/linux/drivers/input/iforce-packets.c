/*
 * $Id$
 *
 *  Copyright (c) 2000-2001 Vojtech Pavlik <vojtech@ucw.cz>
 *  Copyright (c) 2001 Johann Deneux <deneux@ifrance.com>
 *
 *  USB/RS232 I-Force joysticks and wheels.
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Should you need to contact me, the author, you can do so either by
 * e-mail - mail your message to <vojtech@ucw.cz>, or by paper mail:
 * Vojtech Pavlik, Simunkova 1594, Prague 8, 182 00 Czech Republic
 */

#include "iforce.h"

static struct {
	__s32 x;
	__s32 y;
} iforce_hat_to_axis[16] = {{ 0,-1}, { 1,-1}, { 1, 0}, { 1, 1}, { 0, 1}, {-1, 1}, {-1, 0}, {-1,-1}};


void iforce_dump_packet(char *msg, u16 cmd, unsigned char *data)
{
	int i;

	printk(KERN_DEBUG "iforce.c: %s ( cmd = %04x, data = ", msg, cmd);
	for (i = 0; i < LO(cmd); i++)
		printk("%02x ", data[i]);
	printk(")\n");
}

/*
 * Send a packet of bytes to the device
 */
int iforce_send_packet(struct iforce *iforce, u16 cmd, unsigned char* data)
{
	/* Copy data to buffer */
	int n = LO(cmd);
	int c;
	int empty;
	int head, tail;
	unsigned long flags;
			
/*
 * Update head and tail of xmit buffer
 */
	spin_lock_irqsave(&iforce->xmit_lock, flags);

	head = iforce->xmit.head;
	tail = iforce->xmit.tail;

	if (CIRC_SPACE(head, tail, XMIT_SIZE) < n+2) {
		printk(KERN_WARNING "iforce.c: not enough space in xmit buffer to send new packet\n");
		spin_unlock_irqrestore(&iforce->xmit_lock, flags);
		return -1;
	}

	empty = head == tail;
	XMIT_INC(iforce->xmit.head, n+2);

/*
 * Store packet in xmit buffer
 */
	iforce->xmit.buf[head] = HI(cmd);
	XMIT_INC(head, 1);
	iforce->xmit.buf[head] = LO(cmd);
	XMIT_INC(head, 1);

	c = CIRC_SPACE_TO_END(head, tail, XMIT_SIZE);
	if (n < c) c=n;

	memcpy(&iforce->xmit.buf[head],
	       data,
	       c);
	if (n != c) {
		memcpy(&iforce->xmit.buf[0],
		       data + c,
		       n - c);
	}
	XMIT_INC(head, n);

	spin_unlock_irqrestore(&iforce->xmit_lock, flags);
/*
 * If necessary, start the transmission
 */
	switch (iforce->bus) {

#ifdef IFORCE_232
		case IFORCE_232:
		if (empty)
			iforce_serial_xmit(iforce);
		break;
#endif
#ifdef IFORCE_USB
		case IFORCE_USB: 

		if (empty & !iforce->out.status) {
			iforce_usb_xmit(iforce);
		}
		break;
#endif
	}
	return 0;
}

/* Mark an effect that was being updated as ready. That means it can be updated
 * again */
static int mark_core_as_ready(struct iforce *iforce, unsigned short addr)
{
	int i;
	for (i=0; i<iforce->dev.ff_effects_max; ++i) {
		if (test_bit(FF_CORE_IS_USED, iforce->core_effects[i].flags) &&
		    (iforce->core_effects[i].mod1_chunk.start == addr ||
		     iforce->core_effects[i].mod2_chunk.start == addr)) {
			clear_bit(FF_CORE_UPDATE, iforce->core_effects[i].flags);
printk(KERN_DEBUG "iforce.c: marked effect %d as ready\n", i);
			return 0;
		}
	}
	printk(KERN_DEBUG "iforce.c: unused effect %04x updated !!!\n", addr);
	return -1;
}

void iforce_process_packet(struct iforce *iforce, u16 cmd, unsigned char *data)
{
	struct input_dev *dev = &iforce->dev;
	int i;
	static int being_used = 0;

	if (being_used)
		printk(KERN_WARNING "iforce.c: re-entrant call to iforce_process %d\n", being_used);
	being_used++;

#ifdef IFORCE_232
	if (HI(iforce->expect_packet) == HI(cmd)) {
		iforce->expect_packet = 0;
		iforce->ecmd = cmd;
		memcpy(iforce->edata, data, IFORCE_MAX_LENGTH);
		if (waitqueue_active(&iforce->wait))
			wake_up(&iforce->wait);
	}
#endif

	if (!iforce->type) {
		being_used--;
		return;
	}

	switch (HI(cmd)) {

		case 0x01:	/* joystick position data */
		case 0x03:	/* wheel position data */

			if (HI(cmd) == 1) {
				input_report_abs(dev, ABS_X, (__s16) (((__s16)data[1] << 8) | data[0]));
				input_report_abs(dev, ABS_Y, (__s16) (((__s16)data[3] << 8) | data[2]));
				input_report_abs(dev, ABS_THROTTLE, 255 - data[4]);
				if (LO(cmd) >= 8 && test_bit(ABS_RUDDER ,dev->absbit))
					input_report_abs(dev, ABS_RUDDER, (__s8)data[7]);
			} else {
				input_report_abs(dev, ABS_WHEEL, (__s16) (((__s16)data[1] << 8) | data[0]));
				input_report_abs(dev, ABS_GAS,   255 - data[2]);
				input_report_abs(dev, ABS_BRAKE, 255 - data[3]);
			}

			input_report_abs(dev, ABS_HAT0X, iforce_hat_to_axis[data[6] >> 4].x);
			input_report_abs(dev, ABS_HAT0Y, iforce_hat_to_axis[data[6] >> 4].y);

			for (i = 0; iforce->type->btn[i] >= 0; i++)
				input_report_key(dev, iforce->type->btn[i], data[(i >> 3) + 5] & (1 << (i & 7)));

			break;

		case 0x02:	/* status report */
			input_report_key(dev, BTN_DEAD, data[0] & 0x02);

			/* Check if an effect was just started or stopped */
			i = data[1] & 0x7f;
			if (data[1] & 0x80) {
				if (!test_and_set_bit(FF_CORE_IS_PLAYED, iforce->core_effects[i].flags)) {
				/* Report play event */
				input_report_ff_status(dev, i, FF_STATUS_PLAYING);	
printk(KERN_DEBUG "iforce.c: effect %d started to play\n", i);
				}
			}
			else {
				if (!test_bit(FF_CORE_SHOULD_PLAY, iforce->core_effects[i].flags)) {
					if (test_and_clear_bit(FF_CORE_IS_PLAYED, iforce->core_effects[i].flags)) {
					/* Report stop event */
					input_report_ff_status(dev, i, FF_STATUS_STOPPED);	
printk(KERN_DEBUG "iforce.c: effect %d stopped to play\n", i);
					}
				}
				else {
printk(KERN_WARNING "iforce.c: effect %d stopped, while it should not\nStarting again\n", i);
					input_report_ff(dev, i, 1);
				}
			}
			if (LO(cmd) > 3) {
				int j;
				for (j=3; j<LO(cmd); j+=2) {
					if (mark_core_as_ready(iforce, data[j] | (data[j+1]<<8)))
						iforce_dump_packet("ff status", cmd, data);
				}
			}
			break;
	}
	being_used--;
}

int iforce_get_id_packet(struct iforce *iforce, char *packet)
{
	DECLARE_WAITQUEUE(wait, current);
	int timeout = HZ; /* 1 second */

	switch (iforce->bus) {

#ifdef IFORCE_USB
		case IFORCE_USB:

			iforce->dr.request = packet[0];
			iforce->ctrl.dev = iforce->usbdev;

			set_current_state(TASK_INTERRUPTIBLE);
			add_wait_queue(&iforce->wait, &wait);

			if (usb_submit_urb(&iforce->ctrl)) {
				set_current_state(TASK_RUNNING);
				remove_wait_queue(&iforce->wait, &wait);
				return -1;
			}

			while (timeout && iforce->ctrl.status == -EINPROGRESS)
				timeout = schedule_timeout(timeout);

			set_current_state(TASK_RUNNING);
			remove_wait_queue(&iforce->wait, &wait);

			if (!timeout) {
				usb_unlink_urb(&iforce->ctrl);
				return -1;
			}

			break;
#endif
#ifdef IFORCE_232
		case IFORCE_232:

			iforce->expect_packet = FF_CMD_QUERY;
			iforce_send_packet(iforce, FF_CMD_QUERY, packet);

			set_current_state(TASK_INTERRUPTIBLE);
			add_wait_queue(&iforce->wait, &wait);

			while (timeout && iforce->expect_packet)
				timeout = schedule_timeout(timeout);

			set_current_state(TASK_RUNNING);
			remove_wait_queue(&iforce->wait, &wait);

			if (!timeout) {
				iforce->expect_packet = 0;
				return -1;
			}

			break;
#endif
	}

	return -(iforce->edata[0] != packet[0]);
}
