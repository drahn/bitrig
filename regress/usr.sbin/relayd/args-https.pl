# test https connection over http relay

use strict;
use warnings;

our %args = (
    client => {
	func => \&http_client,
	ssl => 1,
    },
    relayd => {
	protocol => [ "http",
	    "request header log foo",
	    "response header log bar",
	],
	forwardssl => 1,
	listenssl => 1,

    },
    server => {
	func => \&http_server,
	ssl => 1,
    },
    len => 251,
    md5 => "bc3a3f39af35fe5b1687903da2b00c7f",
);

1;
