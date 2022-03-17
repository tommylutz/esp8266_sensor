#!/usr/bin/python3
from socket import *
import select
import argparse
import os
from datetime import datetime
import requests
import logging
# logging.basicConfig(level=logging.DEBUG)

parser = argparse.ArgumentParser(description='Log UDP key/value messages to disk')
parser.add_argument('--max_device_id', dest='max_device_id', type=int, default=20, help='Maximum device id to expect')
parser.add_argument('--udp_port', dest='udp_port', type=int, default=4210, help='UDP port to listen on')
parser.add_argument('--base_path', dest='base_path', type=str, default='', help='Base file name for writing to files (e.g. /tmp/udplog_)')
parser.add_argument('--openhab_addr', dest='openhab_addr', type=str, default='', help='Openhab3 Address')
parser.add_argument('--openhab_token_file', dest='openhab_token_file', type=str, default='', help='Path to file contains openhab API token')

OPENHAB_API_TOKEN=''

args = parser.parse_args()

if args.openhab_token_file:
  with open(args.openhab_token_file, 'r') as f:
    OPENHAB_API_TOKEN=f.readline().strip()

print("Using udp port {}, max device id {}, base path {}, openhab_addr {}".format(
  args.udp_port, args.max_device_id, args.base_path, args.openhab_addr))

VALID_KEYS=['HUMIDITY', 'TEMP_F', 'HEAT_INDEX']

def set_file_contents(base_path, device_id, key, contents):
  fname = "{base}{id}_{key}.txt".format(base=base_path, id=device_id, key=key)
  fname_tmp = "{}.tmp".format(fname)
  with open(fname_tmp, 'w') as f:
    f.write(contents)
  # print("Set file {} to contents {}".format(fname, contents))
  os.replace(fname_tmp, fname)

def append_csv_row(base_path, device_id, temp, humidity, heat_index):
  with open("{base}{id}.csv".format(base=base_path, id=device_id), 'a') as f:
    f.write("{date},{id},{temp},{humidity},{heat_index}\n".format(
      date=datetime.strftime(datetime.now(), '%c'),
      id=device_id,
      temp=temp,
      humidity=humidity,
      heat_index=heat_index))

def write_measurement_to_file(measurement):
  print("Received measurement: {}".format(measurement))

  device_id = int(measurement['DEVICE_ID'])
  if device_id < 1 or device_id > args.max_device_id:
    print("Invalid device id {}".format(measurement['DEVICE_ID']))

  set_file_contents(args.base_path, device_id, 'TEMP_F', "{}degF".format(round(float(measurement['TEMP_F']), 1)))
  set_file_contents(args.base_path, device_id, 'HUMIDITY', "{}%".format(round(float(measurement['HUMIDITY']), 1)))
  set_file_contents(args.base_path, device_id, 'HEAT_INDEX', "{}degF".format(round(float(measurement['HEAT_INDEX']), 1)))
  append_csv_row(args.base_path, device_id,
                 round(float(measurement['TEMP_F']), 1),
                 round(float(measurement['HUMIDITY']), 1),
                 round(float(measurement['HEAT_INDEX']), 1))


def update_item_state_openhab(point, url):
  #import http.client
  #http.client.HTTPConnection.debuglevel = 1
  for f in [("temp_f", "TempF"), ("humidity", "Humidity"), ("heat_index", "HeatIndex")]:
    item_name = "UdpLogger{}_{}".format(point["tags"]["device_id"], f[1])
    full_url = "{}/rest/items/{}/state".format(url, item_name)
    value = point["fields"][f[0]]
    print("Putting {} to {}".format(value, full_url))
    res = requests.put(
      full_url,
      auth=(OPENHAB_API_TOKEN, ''),
      headers={'Content-Type': 'text/plain'},
      data="{}".format(value))
    print("Got res: {}".format(res))

ITEMS = {
  "TempF" : {
    "template": 'Number UdpLogger{num}_TempF "UDP{num} Temperature" <temperature> {{ expire="1h0m0s,UNDEF", stateDescription=" "[pattern="%.1f °F",readOnly="true"] }}'
  },
  "Humidity" : {
    "template": 'Number UdpLogger{num}_Humidity "UDP{num} Humidity" <Humidity> {{ expire="1h0m0s,UNDEF", stateDescription=" "[pattern="%.1f %%",readOnly="true"] }}'
  },
  "HeatIndex" : {
    "template": 'Number UdpLogger{num}_HeatIndex "UDP{num} Heat Index" <temperature> {{ expire="1h0m0s,UNDEF", stateDescription=" "[pattern="%.1f °F",readOnly="true"] }}'
  }
}

def print_item_definitions():
  for i in range(1,args.max_device_id+1):
    for key,props in ITEMS.items():
      print(props["template"].format(num=i))

def ResolveLoc(num):
  mappings = {
    1: "Garage",
    2: "Patio",
    3: "Lower Attic",
    4: "Workshop",
    5: "Upper Attic",
    10: "Outside",
  }
  if num in mappings:
    return mappings[num]
  return "{}".format(num)

def print_sitemap_page():
  for i in range(1,args.max_device_id+1):
    print('        Text icon="temperature" item=UdpLogger{num}_TempF label="{loc} Temp"'.format(num=i, loc=ResolveLoc(i)))
    print('        Text icon="Humidity" item=UdpLogger{num}_Humidity label="{loc} Hum."'.format(num=i, loc=ResolveLoc(i)))

def main():
  if args.openhab_addr:
    print_item_definitions()
    print_sitemap_page()

  s = socket(AF_INET, SOCK_DGRAM)
  s.bind(('', 4210))

  while True:
    ready = select.select([s], [], [], 30)
    if ready[0]:
      msg, addr = s.recvfrom(1024)
      measurement = {q[0] : q[1] for q in
                     [p.split('=') for  p in msg.decode('ascii').split(',')] }
      if args.base_path:
        write_measurement_to_file(measurement)
      point = {
              "measurement": "sensor",
              "tags": {
                  "device_id": measurement["DEVICE_ID"],
              },
              "fields": {
                  "temp_f" : float(measurement["TEMP_F"]),
                  "humidity" : float(measurement["HUMIDITY"]),
                  "heat_index" : float(measurement["HEAT_INDEX"]),
              }
          }
      if args.openhab_addr:
        update_item_state_openhab(point, args.openhab_addr)


    else:
      print("Timed out waiting for data")

if __name__ == '__main__':
  main()
  
