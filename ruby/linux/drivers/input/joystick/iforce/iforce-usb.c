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

void iforce_usb_xmit(struct iforce *iforce)
{
	int n, c;
	unsigned long flags;

	spin_lock_irqsave(&iforce->xmit_lock, flags);

	if (iforce->xmit.head == iforce->xmit.tail) {
		spin_unlock_irqrestore(&iforce->xmit_lock, flags);
		return;
	}

	((char *)iforce->out.transfer_buffer)[0] = iforce->xmit.buf[iforce->xmit.tail];
	XMIT_INC(iforce->xmit.tail, 1);
	n = iforce->xmit.buf[iforce->xmit.tail];
	XMIT_INC(iforce->xmit.tail, 1);

	iforce->out.transfer_buffer_length = n + 2;
	iforce->out.dev = iforce->usbdev;

	/* Copy rest of data then */
	c = CIRC_CNT_TO_END(iforce->xmit.head, iforce->xmit.tail, XMIT_SIZE);
	if (n < c) c=n;

	memcpy(iforce->out.transfer_buffer + 1,
	       &iforce->xmit.buf[iforce->xmit.tail],
	       c);
	if (n != c) {
		memcpy(iforce->out.transfer_buffer + 1 + c,
		       &iforce->xmit.buf[0],
		       n-c);
	}
	XMIT_INC(iforce->xmit.tail, n);

	if ( (n=usb_submit_urb(&iforce->out)) ) {
		printk(KERN_WARNING "iforce.c: iforce_usb_xmit: usb_submit_urb failed %d\n", n);
	}

	spin_unlock_irqrestore(&iforce->xmit_lock, flags);
}

static void iforce_usb_irq(struct urb *urb)
{
	struct iforce *iforce = urb->context;
	if (urb->status) return;
	iforce_process_packet(iforce,
		(iforce->data[0] << 8) | (urb->actual_length - 1), iforce->data + 1);
}

static void iforce_usb_out(struct urb *urb)
{
	struct iforce *iforce = urb->context;

	if (urb->status) return;

	iforce_usb_xmit(iforce);

	if (waitqueue_active(&iforce->wait))
		wake_up(&iforce->wait);
}

static void iforce_usb_ctrl(struct urb *urb)
{
	struct iforce *iforce = urb->context;
	if (urb->status) return;
	iforce->ecmd = 0xff00 | urb->actual_length;
	if (waitqueue_active(&iforce->wait))
		wake_up(&iforce->wait);
}

static void *iforce_usb_probe(struct usb_device *dev, unsigned int ifnum,
				const struct usb_device_id *id)
{
	struct usb_endpoint_descriptor *epirq, *epout;
	struct iforce *iforce;

	epirq = dev->config[0].interface[ifnum].altsetting[0].endpoint + 0;
	epout = dev->config[0].interface[ifnum].altsetting[0].endpoint + 1;

	if (!(iforce = kmalloc(sizeof(struct iforce) + 32, GFP_KERNEL))) return NULL;
	memset(iforce, 0, sizeof(struct iforce));

	iforce->bus = IFORCE_USB;
	iforce->usbdev = dev;

	iforce->cr.bRequestType = USB_TYPE_VENDOR | USB_DIR_IN | USB_RECIP_INTERFACE;
	iforce->cr.wIndex = 0;
	iforce->cr.wLength = 16;

	FILL_INT_URB(&iforce->irq, dev, usb_rcvintpipe(dev, epirq->bEndpointAddress),
			iforce->data, 16, iforce_usb_irq, iforce, epirq->bInterval);

	FILL_BULK_URB(&iforce->out, dev, usb_sndbulkpipe(dev, epout->bEndpointAddress),
			iforce + 1, 32, iforce_usb_out, iforce);

	FILL_CONTROL_URB(&iforce->ctrl, dev, usb_rcvctrlpipe(dev, 0),
			(void*) &iforce->cr, iforce->edata, 16, iforce_usb_ctrl, iforce);

	if (iforce_init_device(iforce)) {
		kfree(iforce);
		return NULL;
	}

	return iforce;
}

static void iforce_usb_disconnect(struct usb_device *dev, void *ptr)
{
	struct iforce *iforce = ptr;
	usb_unlink_urb(&iforce->irq);
	input_unregister_device(&iforce->dev);
#if 0
	kfree(iforce);
#endif
}

static struct usb_device_id iforce_usb_ids [] = {
	{ USB_DEVICE(0x044f, 0xa01c) },		/* Thrustmaster Motor Sport GT */
	{ USB_DEVICE(0x046d, 0xc281) },		/* Logitech WingMan Force */
	{ USB_DEVICE(0x046d, 0xc291) },		/* Logitech WingMan Formula Force */
	{ USB_DEVICE(0x05ef, 0x020a) },		/* AVB Top Shot Pegasus */
	{ USB_DEVICE(0x05ef, 0x8884) },		/* AVB Mag Turbo Force */
	{ USB_DEVICE(0x05ef, 0x8888) },		/* AVB Top Shot FFB Racing Wheel */
	{ USB_DEVICE(0x06f8, 0x0001) },		/* Guillemot Race Leader Force Feedback */
	{ USB_DEVICE(0x06f8, 0x0004) },		/* Guillemot Force Feedback Racing Wheel */
	{ }					/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, iforce_usb_ids);

struct usb_driver iforce_usb_driver = {
	name:		"iforce",
	probe:		iforce_usb_probe,
	disconnect:	iforce_usb_disconnect,
	id_table:	iforce_usb_ids,
};
