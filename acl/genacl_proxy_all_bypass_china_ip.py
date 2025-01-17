#!/usr/bin/env python3

# modified from https://github.com/shadowsocks/shadowsocks-rust/blob/master/acl/genacl_proxy_gfw_bypass_china_ip.py
# use white list mode(proxy_all and bypass_list)

from urllib import request, parse
import logging
import sys
import json
from datetime import datetime

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

CHINA_IP_LIST_URL = "https://raw.githubusercontent.com/17mon/china_ip_list/master/china_ip_list.txt"
CUSTOM_BYPASS = [
    "127.0.0.1",
    "10.0.0.0/8",
    "172.16.0.0/12",
    "192.168.0.0/16",
    "fd00::/8",
]

def fetch_url_content(url):
    logger.info("FETCHING {}".format(url))
    r = request.urlopen(url)
    return r.read()


def write_china_ip(fp):
    china_ip_list = fetch_url_content(CHINA_IP_LIST_URL)
    fp.write(china_ip_list)
    fp.write(b"\n")


try:
    output_file_path = sys.argv[1]
except:
    output_file_path = "pegasocks.acl"

logger.info("WRITING {}".format(output_file_path))

with open(output_file_path, 'wb') as fp:
    now = datetime.now()

    fp.write(b"# Generated by genacl.py\n")
    fp.write("# Time: {}\n".format(now.isoformat()).encode("utf-8"))
    fp.write(b"\n")

    fp.write(b"[proxy_all]\n")
    fp.write(b"\n[bypass_list]\n")
    write_china_ip(fp)

    if len(CUSTOM_BYPASS) > 0:
        logger.info("CUSTOM_BYPASS {} lines".format(len(CUSTOM_BYPASS)))
        fp.write(b"\n[bypass_list]\n")
        for a in CUSTOM_BYPASS:
            fp.write(a.encode("utf-8"))
            fp.write(b"\n")

logger.info("DONE")
