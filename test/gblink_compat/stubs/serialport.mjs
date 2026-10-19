// Stand-in for 'serialport': the ESP32 Switch bridge path is not exercised.
export class SerialPort { static async list() { return []; } }
