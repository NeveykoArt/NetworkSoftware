# NetworkSoftware

Lab1
UDP server with confirmation.
Client waits until the server sends an ACK packet.
Any type of file can be sent.
Up to 65,536 clients can be served (limited by the number of file descriptors).

Usage:
./server ip_address [packet_positions_to_loose]
./client server_ip_address server_port filename

Example:
./server 127.0.0.1 [1,1,1,7,7777,7]   ->   1st packet will be lost up to 3 times, 7th packet - 2 times and 777th packet - once
./client 127.0.0.1 server_port test1.jpg
