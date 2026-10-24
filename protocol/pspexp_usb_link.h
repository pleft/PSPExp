/*
 * PSPExp USB link: the byte-stream layer under the protocol, shared by the
 * PSP kernel driver (psp/transport-usb) and the host transport
 * (host/transport-usb). Section 14: nothing here leaks into the protocol.
 *
 * The PSP is the USB device (vendor-specific class, one interface, two bulk
 * endpoints). Each direction is a byte stream carried as a sequence of bulk
 * transfers. The receiver always posts PSPEXP_USB_CHUNK-byte reads, which
 * complete on a short packet or when full. The sender therefore guarantees
 * that every transfer ends with a short packet: a transfer whose length is
 * a multiple of the 512-byte hi-speed packet size is split into two
 * transfers (length - PSPEXP_USB_SPLIT and PSPEXP_USB_SPLIT), which relies
 * on nothing beyond ordinary bulk semantics and needs no zero-length
 * packets. The stream is unaffected because both halves arrive in order.
 *
 * Connection: the host sends a PSPEXP_USB_SYNC_LEN-byte sync packet as its
 * own transfer. The driver drops any stale state, answers with the same
 * packet and reports "connected" to the runtime, which then sends HELLO.
 * A sync packet arriving mid-stream (host process restarted) resets the link
 * the same way. The protocol header starts with "PX" so no protocol chunk
 * can be mistaken for a sync packet.
 */
#ifndef PSPEXP_USB_LINK_H
#define PSPEXP_USB_LINK_H

#include <stdint.h>

#define PSPEXP_USB_VID          0x054Cu   /* Sony; the PSP bus driver supplies it */
#define PSPEXP_USB_PID          0x01CAu   /* distinct from mass storage 0x1C8 and PSPLink 0x1C9 */
#define PSPEXP_USB_DRIVER_NAME  "PSPExpUSBDriver"
/* PSPExp Capture's plugin (psp/capture) presents the same link under a product of its own, so a
 * board's console, waiting for PSPExp, never takes a native game's capture, and the receiver
 * (tools/pspexp-capture) runs beside it */
#define PSPEXP_CAPTURE_USB_PID  0x01CBu
#define PSPEXP_CAPTURE_USB_DRIVER_NAME "PSPExpCapture"
#define PSPEXP_USB_EP_IN        0x81u     /* PSP -> host */
#define PSPEXP_USB_EP_OUT       0x02u     /* host -> PSP */
#define PSPEXP_USB_PACKET_HS    512u
#define PSPEXP_USB_PACKET_FS    64u
#define PSPEXP_USB_CHUNK        (64u * 1024u)   /* receive request size, both ends */
#define PSPEXP_USB_SPLIT        16u

#define PSPEXP_USB_SYNC_LEN     16
/* "PSPEXPSYNC" + version + zero padding. Must not start with "PX". */
#define PSPEXP_USB_SYNC_BYTES   { 'S','Y','N','C','P','S','P','E','X','P',0x01,0,0,0,0,0 }

#endif /* PSPEXP_USB_LINK_H */
