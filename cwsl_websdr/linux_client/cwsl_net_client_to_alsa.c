/*
  CWSL_Net Client
 
  (c)20.03.2015 Hardy Lau  DL1GLH
  (c)11.01.2017

  Does Provide ALSA-Access to CWSL_Net to feed via snd-aloop into websdr
*/

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

// Buffer size in samples
#define BUFFER_SIZE 2048
// Data size in bytes
#define DATA_SIZE 1400

#define LISTEN_PORT 11000
#define UDP_BASE_PORT 52000

#include <alsa/asoundlib.h>
#include <math.h>

char device[40]; // = "hw:Loopback,0,0"; /* playback device */

volatile int ende = 0; 

void do_on_signal( int signo)
{
 ende = 1;
}

int main(int argc, char**argv)
{
   int error;
   int loopback;
   int channel;
   int sockfd, n;
   int optval;
   socklen_t optlen = sizeof(optval);
   struct sockaddr_in servaddr,cliaddr;
   char sendline[1000];
   char recvline[1000];
   int udp_sockfd;
   snd_pcm_t *handle;

   struct sockaddr_in udp_servaddr, udp_cliaddr;
   socklen_t len;
   char mesg[BUFFER_SIZE*2*sizeof(short)];
   int i,j;
   int sf;

   int err;
   unsigned long k;
   snd_pcm_sframes_t frames;

   long frames_counter = 0;
   long frames_counter_avg = 0;
   int sekunden = -1;
   time_t zeit, zeit_alt;

   if (argc != 5)
   {
      printf("usage: <IP address> <BAND> <ScalingFactor> D|I\n");
      exit(1);
   }

   // Which Channel - BAND
   sscanf( argv[2], "%d", &channel);
   printf( "Channel: %d\n", channel);

   // Scaling Factor
   sscanf( argv[3], "%d", &sf);

   // Daemon?
   if( *argv[4] == 'D') {
     if( fork() == 0) { //Client
       chdir( "/");
       close(0);
       open( "/dev/null", O_RDONLY );
       close(1);
       open( "/dev/null", O_WRONLY);
       close(2);
       open("/dev/null", O_WRONLY);
     } else { //Parent
       exit(0);
     }
   }

   // Connect to Server and send commands
   sockfd=socket(AF_INET,SOCK_STREAM,0);

   /* Set the SO_KEEPALIVE option active */
   optval = 1;
   optlen = sizeof(optval);
   setsockopt( sockfd, SOL_SOCKET, SO_KEEPALIVE, &optval, optlen);

   bzero(&servaddr,sizeof(servaddr));
   servaddr.sin_family = AF_INET;
   servaddr.sin_addr.s_addr=inet_addr(argv[1]);
   servaddr.sin_port=htons(LISTEN_PORT);
   connect(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr));

   //Open ALSA
   sprintf( device, "hw:Loopback,0,%d", channel);
   if ((err = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
     printf("Playback open error: %s\n", snd_strerror(err));
     exit(EXIT_FAILURE);
   }
   if ((err = snd_pcm_set_params(handle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 192000, 1, 500000L)) < 0) { /* 0.5sec */
     printf("Playback open error: %s\n", snd_strerror(err));
     exit(EXIT_FAILURE);
   }

   //Prepare UDP-Receiver
   udp_sockfd=socket(AF_INET,SOCK_DGRAM,0);
   bzero(&udp_servaddr,sizeof(udp_servaddr));
   udp_servaddr.sin_family = AF_INET;
   udp_servaddr.sin_addr.s_addr=htonl(INADDR_ANY);
   udp_servaddr.sin_port=htons(UDP_BASE_PORT+channel); 
   bind(udp_sockfd,(struct sockaddr *)&udp_servaddr,sizeof(udp_servaddr));

   //Send Attach- and Start iq - Commands
   sprintf( sendline, "attach %d\n", channel);
   sendto(sockfd,sendline,strlen(sendline),0, (struct sockaddr *)&servaddr,sizeof(servaddr));
   n=recvfrom(sockfd,recvline,10000,0,NULL,NULL);
   recvline[n]=0;
   fputs(recvline,stdout);

   sprintf( sendline, "start iq %d %d\n", UDP_BASE_PORT+channel, sf);
   sendto(sockfd,sendline,strlen(sendline),0, (struct sockaddr *)&servaddr,sizeof(servaddr));
   n=recvfrom(sockfd,recvline,10000,0,NULL,NULL);
   recvline[n]=0;
   fputs(recvline,stdout);
   
   printf( "\n");
   fflush(stdout);

   //close IQ on signal()
   signal( SIGHUP, do_on_signal);
   signal( SIGTERM, do_on_signal);

   zeit_alt = zeit = time(NULL);

   //Process IQ-Data
   while (!ende)
   {
      //Check if TCP-Socket is still alive
      socklen_t len = sizeof (error);
      if( getsockopt (sockfd, SOL_SOCKET, SO_ERROR, &error, &len ) < 0) break;

      //Copy data
      len = sizeof(udp_cliaddr);
      n = recvfrom(udp_sockfd,(char*)&mesg,sizeof(mesg),0,(struct sockaddr *)&udp_cliaddr,&len);
      if( n < 0) { printf( "recvfrom error\n"); break; }
  
        frames = snd_pcm_writei(handle, mesg, n/(2*sizeof(short)));
        if (frames < 0)
          frames = snd_pcm_recover(handle, frames, 0);
        if (frames < 0) {
          printf("snd_pcm_write failed: %s\n", snd_strerror(err));
          break;
        }
        if (frames > 0 && frames < n/(2*sizeof(short))) {
          printf("Short write (expected %d, wrote %l)\n", n, frames);
        } 
   }

   //Send Detach- and Stop iq and quit - Commands
   sprintf( sendline, "stop iq %d\n", 0);
   sendto(sockfd,sendline,strlen(sendline),0, (struct sockaddr *)&servaddr,sizeof(servaddr));
   n=recvfrom(sockfd,recvline,10000,0,NULL,NULL);
   recvline[n]=0;
   fputs(recvline,stdout);

   sprintf( sendline, "detach %d\n", UDP_BASE_PORT+0);
   sendto(sockfd,sendline,strlen(sendline),0, (struct sockaddr *)&servaddr,sizeof(servaddr));
   n=recvfrom(sockfd,recvline,10000,0,NULL,NULL);
   recvline[n]=0;
   fputs(recvline,stdout);

   sprintf( sendline, "quit\n");
   sendto(sockfd,sendline,strlen(sendline),0, (struct sockaddr *)&servaddr,sizeof(servaddr));
   n=recvfrom(sockfd,recvline,10000,0,NULL,NULL);
   recvline[n]=0;
   fputs(recvline,stdout); 
   
   //Stop IQ-UDP
   close( udp_sockfd);

   //Close ALSA
   snd_pcm_close(handle);

}
