import express from "express";
import bodyParser from "body-parser";
import fs from "fs";
import dgram from "dgram";

const app = express();
const HTTP_PORT = 5003;
const UDP_PORT = 5003;
const HOST = "0.0.0.0";

// ─── UDP LISTENER ──────────────────────────────────────────────────────────────
const udpServer = dgram.createSocket("udp4");

udpServer.on("listening", () => {
  // tslint:disable-next-line:no-console
  console.log(`UDP server listening on ${HOST}:${UDP_PORT}`);
});

udpServer.on("message", (msg, rinfo) => {
  // tslint:disable-next-line:no-console
  console.log(
    `Got ${msg.length} UDP bytes from ${rinfo.address}:${rinfo.port}`
  );
  fs.appendFile("i2s.raw", msg, (err) => {
    if (err) {
      // tslint:disable-next-line:no-console
      console.error("Error writing UDP data to i2s.raw:", err);
    }
  });
});

udpServer.bind(UDP_PORT, HOST);

// ─── EXPRESS (HTTP) SERVER ────────────────────────────────────────────────────
app.use(
  bodyParser.raw({
    type: "*/*",
  })
);

// route to handle samples from the ADC - 16 bit single channel samples
app.post("/adc_samples", (req, res) => {
  // tslint:disable-next-line:no-console
  console.log(`Got ${req.body.length} ADC bytes (HTTP)`);
  fs.appendFile("adc.raw", req.body, (err) => {
    if (err) {
      // tslint:disable-next-line:no-console
      console.error("Error writing ADC data:", err);
      return res.status(500).send("Error");
    }
    res.send("OK");
  });
});

// route to handle samples from the I2S microphones - 32 bit stereo channel samples
app.post("/i2s_samples", (req, res) => {
  // tslint:disable-next-line:no-console
  console.log(`Got ${req.body.length} I2S bytes (HTTP)`);
  fs.appendFile("i2s.raw", req.body, (err) => {
    if (err) {
      // tslint:disable-next-line:no-console
      console.error("Error writing I2S data:", err);
      return res.status(500).send("Error");
    }
    res.send("OK");
  });
});

app.listen(HTTP_PORT, HOST, () => {
  // tslint:disable-next-line:no-console
  console.log(`Express HTTP server started at http://${HOST}:${HTTP_PORT}`);
});
