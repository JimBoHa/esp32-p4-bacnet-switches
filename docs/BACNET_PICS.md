# BACnet implementation summary

This is an engineering PICS-style summary for firmware 1.19.0. It is not a BTL
test report, certification, or listing claim.

| Item | Implementation |
|---|---|
| Data link | BACnet/IPv4 over onboard IP101GRI Ethernet |
| Default UDP port | 47808 (`0xBAC0`), persistent configurable |
| Default Device instance | 599152, persistent configurable |
| Protocol version / revision | 1 / 17 |
| Maximum APDU | 1476 octets |
| Segmentation | Not supported |
| COV capacity | Eight simultaneous subscriptions |
| BBMD / Foreign Device | Not implemented |
| BACnet/SC | Not implemented |

Local and directed Who-Is receive I-Am. Direct requests receive direct replies;
broadcast/global and forwarded-broadcast discovery receive broadcast replies,
including the form tested with Metasys. Cross-subnet discovery still needs an
external BACnet router/BBMD design.

## Services executed as server

- Who-Is / I-Am
- Who-Has by object identifier or exact configured name / I-Have
- confirmed ReadProperty
- confirmed ReadPropertyMultiple, including All/Required/Optional selectors
- confirmed SubscribeCOV with confirmed or unconfirmed notifications

All objects are read-only. WriteProperty and unsupported confirmed services are
rejected. Alarm/event services, time synchronization, file transfer,
DeviceCommunicationControl, ReinitializeDevice, and private transfer are not
implemented.

## Objects

- Device: configured instance (default 599152)
- Binary Input 20, 21, 22: debounced physical GPIO inputs
- Binary Input 1001: Ethernet link state
- Binary Input 1002: IPv4 assigned state
- Analog Value 1000: chip die temperature, degrees Celsius
- Analog Value 1001: boot uptime, seconds
- Analog Value 1002/1003: current/minimum free heap, bytes
- Analog Value 1004/1005: Ethernet link losses/reconnects
- Analog Value 1006/1007: BACnet received packets/protocol errors
- Analog Value 1008: numeric ESP-IDF reset reason
- Analog Value 1009: active COV subscription count
- Analog Value 1010: persistent boot count
- Network Port 1: live read-only BACnet/IP and Ethernet state

Binary Inputs implement Present_Value, Status_Flags, Event_State,
Out_Of_Service, Polarity, Reliability, active/inactive text, Description, and
Property_List. Physical and network-status BIs support COV. Analog Values expose
read-only diagnostic values and status metadata. Network Port exposes IPv4,
UDP port, DHCP, subnet, gateway, DNS, BACnet/IP MAC address, link speed,
reliability, and protocol/link state.

The Device object exposes standard identity, object/service bit strings,
Object_List, Database_Revision, restart reason, firmware revision, and an
Application_Software_Version containing version plus source revision. Fixed and
configured Object_Name values must be unique case-insensitively so Who-Has is
unambiguous.

Vendor ID 999 and `Lab placeholder` are private-lab defaults. Replace them with
the organization's assigned ASHRAE identity before product distribution.
