// Stand-in for the 'usb' (node-usb) package, so GB-Link's netplay-bridge
// engine bundles without libusb. Nothing here is reached: every test hands the
// engine a SimTransport instead of letting it open a real device.
export const usb = { on() {}, removeListener() {}, unrefHotplugEvents() {} };
export const findByIds = () => undefined;
export const getDeviceList = () => [];
export class Device {}
export class Interface {}
export class InEndpoint {}
export class OutEndpoint {}
