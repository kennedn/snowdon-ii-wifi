# Infrared NEC

The project implements a minimal HTTP server that exposes the following RESTful API endpoint:

http://<ip_address>:8080/ir

The endpoint expects a single `code` parameter, which can be sent via either url encoding or in the JSON body of the request, e.g:

```bash
curl -X PUT http://192.168.1.238:8080/ir?code=807F807F
# or
curl -X PUT http://192.168.1.238:8080/ir -H 'Content-Type: application/json' -d '{"code": "807F807F"}'
```

A very hacky NEC driver is implemented in PIOASM. The `code` variable is translated to a NEC encoded stream and transmitted over a connected IR LED on GPIO16.

The PIOASM driver implements both the transmission of data and a 38kHz modulation to produce the carrier wave on a single state machine. This project was my own delve into the protocol and was pieced together from skimming the data sheet and observing oscilloscope output. 

As a result is is not the most efficient way of achieving the NEC protocol on the pico (coming in a whopping 30 instructions!) and I would suggest taking a look at the code found in the official [pico-examples repository](https://github.com/raspberrypi/pico-examples/tree/master/pio/ir_nec/nec_transmit_library). This example implements the same protocol across two state machines using IRQ, something I long thought possible but had insufficient examples of state machine IRQ to attempt it when I originally wrote the driver.