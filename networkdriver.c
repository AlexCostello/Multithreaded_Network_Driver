/*
 * NAME: Kyle Holmberg
 * DUCKID: kmh
 * SIN: 951312729
 * CLASS: CIS415
 *
 * Project 2
 * 
 * This file consists entirely of my original work
 */

#include <stdlib.h>
#include <stdio.h>
#include <sched.h>
#include <pthread.h>

#include "packetdescriptor.h"
#include "destination.h"
#include "pid.h"
#include "freepacketdescriptorstore.h"
#include "freepacketdescriptorstore__full.h"
#include "networkdevice.h"
#include "packetdescriptorcreator.h"
#include "networkdevice.h"
#include "BoundedBuffer.h"
#include "diagnostics.h"

static void* send_thread();
static void* receive_thread();

/* 36 Packets */
#define BUFFERSIZE 10
#define RECPOOLSIZE 4
#define RECIEVE_BUFFERSIZE 2

#define RECEIVAL_COUNTER 0


FreePacketDescriptorStore *fpds;
NetworkDevice *netdev;
BoundedBuffer *bufferArray[MAX_PID+1];
BoundedBuffer *sendQueue;
BoundedBuffer *recPool;

void init_network_driver(NetworkDevice *nd, 
						 void *mem_start, 
						 unsigned long mem_length, 
						 FreePacketDescriptorStore **fpds_ptr) 
{	int i;

	//Initiliaze Device
	netdev = nd; //Assign network device from argument.

	//Initialize Threads before creation
	pthread_t sendThread;
	pthread_t receiveThread;

	//To catch packets as soon as they send.
	PacketDescriptor* pd;

	/* Create Free Packet Descriptor Store */
	fpds = create_fpds();
	*fpds_ptr = fpds;

	/* Load FPDS with packet descriptors constructed from mem_start/mem_length */
	create_free_packet_descriptors(fpds, mem_start, mem_length);

	/* Create buffers required by your thread[s] */ 
	sendQueue = createBB(BUFFERSIZE);
	for (i = 0; i <= MAX_PID; i++) {
		bufferArray[i] = createBB(RECIEVE_BUFFERSIZE);
	}
	recPool = createBB(RECPOOLSIZE);
	for (i = 0; i < RECPOOLSIZE; i++) {
		blocking_get_pd(fpds, &pd);
		blockingWriteBB(recPool, pd);
	}

	/* Create Threads */
	pthread_create(&sendThread, NULL, send_thread, NULL);
	pthread_create(&receiveThread, NULL, receive_thread, NULL);
}

/* 
 * Method to send packets 
 */
static void* send_thread()
{
	int sendLimit = 5;
	int i;
	PacketDescriptor* temppd;
	while (1) {
		temppd = (PacketDescriptor*)blockingReadBB(sendQueue);
		
		for (i = 0; i < sendLimit; i++) { // Attempt to send 5 times
			if ((send_packet(netdev, temppd)) == 1) { // If received, break.
				DIAGNOSTICS("[DRIVER> Info: Sent a packet after %d tries", i);
				break;			
			}
			//Potentially need a way to wait before next send attempt
		}
		if (nonblockingWriteBB(recPool, temppd) != 1) {
			if (nonblocking_put_pd(fpds, temppd) != 1) {
				DIAGNOSTICS("DRIVER> Error? Failed to return Packet Descriptor to store");
			}
		}
	}
	return NULL;
}

/* 
 * Method to receive packets 
 */
static void* receive_thread()
{
	PacketDescriptor* current_pd;
	PacketDescriptor* filled_pd;
	PID procID;

	/* First, receive the thread as fast as possible to allow work to continue */
	blocking_get_pd(fpds, &current_pd); // Prepare packet from store
	init_packet_descriptor(current_pd); // Reset packet descriptor before registering it to device.
	register_receiving_packetdescriptor(netdev, current_pd); //Register the packet with the device.

	/* Thread receival complete: Handle PD */
	while(1) {
		await_incoming_packet(netdev); //Waits until pd filled with data
		filled_pd = current_pd; //Packet Descriptor is now filled.
		RECEIVAL_COUNTER++
		DIAGNOSTICS("\n\nReceival Count =" + RECEIVAL_COUNTER + "\n");
		if (nonblockingReadBB(recPool, &current_pd) == 1) { //Something is in the recPool
			init_packet_descriptor(current_pd); //Resets current_pd - it is now empty
			register_receiving_packetdescriptor(netdev, current_pd); //Tell the netdev that current_pd is empty
			procID = packet_descriptor_get_pid(filled_pd); //Find process ID for indexing purposes (0-10)			    	
			if (nonblockingWriteBB(bufferArray[procID], filled_pd) != 1) { //Data from packet fails to write
				DIAGNOSTICS("[DRIVER> Warning: bufferArray(%u) - Packet Store full, discarding data.\n", procID);
				if (nonblocking_put_pd(fpds, filled_pd) != 1) { //Can't return packet to fpds
					DIAGNOSTICS("[DRIVER> Error? Cannot return Packet Descriptor to store\n");
				}
			}
		} else if (nonblocking_get_pd(fpds, &current_pd)) { //Something is NOT in the recPool
			init_packet_descriptor(current_pd); //Resets current_pd - it is now empty
			register_receiving_packetdescriptor(netdev, current_pd); //Tell the netdev that current_pd is empty
			procID = packet_descriptor_get_pid(filled_pd); //Find process ID for indexing purposes (0-10)			    	
			if (nonblockingWriteBB(bufferArray[procID], filled_pd) != 1) { //Data from packet fails to write
				DIAGNOSTICS("[DRIVER> Warning: bufferArray(%u) - Packet Store full, discarding data.\n", procID);
				if (nonblocking_put_pd(fpds, filled_pd) != 1) { //Can't return packet to fpds
					DIAGNOSTICS("[DRIVER> Error? Cannot return Packet Descriptor to store\n");
				}
			}
		} else {
			usleep(5);
			if (nonblocking_put_pd(fpds, filled_pd) != 1) { //Can't return packet to fpds
				DIAGNOSTICS("[DRIVER> Warning: Cannot return Packet Descript to fpds");
			}
			printf("[DRIVER> Warning: No replacement Packet Descriptor, discarding data.\n");
			current_pd = filled_pd;
			init_packet_descriptor(current_pd);
			register_receiving_packetdescriptor(netdev, current_pd);
		}
	}
	return NULL;
}

/* 
 * Queue up packet descriptor for sending
 * Do not return until it has been successfully queued
 */
void blocking_send_packet(PacketDescriptor *pd) 
{
	blockingWriteBB(sendQueue, pd);
	return;   
} 

/*
 * If you are able to queue up packet descriptor immediately, do so and return 1
 * Otherwise, return 0 
 */
int nonblocking_send_packet(PacketDescriptor *pd) 
{
	return nonblockingWriteBB(sendQueue, pd);
} 

/* 
 * Wait until there is a packet for 'pid'. 
 * Return that packet descriptor to the calling application.
 */
void blocking_get_packet(PacketDescriptor **pd, PID pid) 
{
	*pd = blockingReadBB(bufferArray[pid]);
	return;
} 

/* 
 * If there is currently a waiting packet for 'pid', 
 * return that packet to the calling application and return 1
 * otherwise, return 0 for the value of the function 
 */
int nonblocking_get_packet(PacketDescriptor **pd, PID pid) 
{
	return nonblockingReadBB(bufferArray[pid], pd);
}
