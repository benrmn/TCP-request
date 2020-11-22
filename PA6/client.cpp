#include "common.h"
#include "BoundedBuffer.h"
#include "Histogram.h"
#include "common.h"
#include "HistogramCollection.h"
#include "TCPreqchannel.h"
#include <time.h>
#include <thread>
#include <sys/epoll.h>
using namespace std;

void timediff (struct timeval& start, struct timeval& end) {
    int secs = (end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)/(int) 1e6;
    int usecs = (int)(end.tv_sec * 1e6 + end.tv_usec - start.tv_sec * 1e6 - start.tv_usec)%((int) 1e6);
    cout << "Took " << secs << " seconds and " << usecs << " micro seconds" << endl;
}

void patient_thread_function(int n, int pno, BoundedBuffer* request_buffer){
    datamsg d (pno, 0.0, 1);
    double resp = 0;
    for (int i = 0; i < n; i++) {
        request_buffer->push((char *) &d, sizeof(datamsg));
        d.seconds += 0.004;
    }
}

void file_thread_function (string fname, BoundedBuffer* request_buffer, TCPRequestChannel* chan, int mb) {
    //1. create the file
    string recvfname = "recv/" + fname;
    // make it as long as original length
    char buf [1024];
    filemsg f (0, 0);
    memcpy (buf, &f, sizeof (f));
    strcpy (buf + sizeof (f), fname.c_str());
    chan->cwrite (buf, sizeof (f) + fname.size() + 1);
    __int64_t filelength;
    chan->cread (&filelength, sizeof (filelength));

    FILE* fp = fopen (recvfname.c_str(), "wb");
    fseek (fp, filelength, SEEK_SET);
    fclose (fp);

    //2. generate all file msgs
    filemsg* fm = (filemsg *) buf;
    __int64_t remlen = filelength;

    while (remlen > 0) {
        fm->length = min (remlen, (__int64_t) mb);
        request_buffer->push (buf, sizeof (filemsg) + fname.size () + 1);
        fm->offset += fm->length;
        remlen -= fm->length;
    }
}

void event_polling_thread (int w, int mb, TCPRequestChannel** wchans, BoundedBuffer* request_buffer, HistogramCollection* hc){
    char buf [1024];
    double resp = 0;

    char recvbuf [mb];

    struct epoll_event ev;
    struct epoll_event events[w];

    // create an empty epoll list
    int epollfd = epoll_create1 (0);
    if (epollfd == -1) {
        EXITONERROR ("epoll_create1");
    }

    unordered_map<int, int> fd_to_index;
    vector<vector<char>> state (w);
    // priming + adding each rfd to the list
    bool quit_recv = false;
    int nsent = 0, nrecv = 0;
    for (int i = 0; i < w; i++) {
        int sz = request_buffer->pop (buf, 1024);
        if (*(MESSAGE_TYPE *) buf == QUIT_MSG) {
            quit_recv = true;
            break;
        }
        wchans[i]->cwrite (buf, sz);
        state [i] = vector<char>(buf, buf+sz); // record the state [i]
        nsent ++;
        int rfd = wchans [i]->getfd();
        fcntl(rfd, F_SETFL, O_NONBLOCK);

        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = rfd;
        fd_to_index [rfd] = i;
        if (epoll_ctl(epollfd, EPOLL_CTL_ADD, rfd, &ev) == -1) {
            EXITONERROR("epoll_ctl");
        }
    }

    // nsent = w, nrecvd = 0;

    while (true) {
        if (quit_recv && nsent == nrecv)
            break;
        int nfds = epoll_wait (epollfd, events, w, -1);
        if (nfds == -1) {
            EXITONERROR("epoll_wait");
        }
        for (int i = 0; i < nfds; i++) {
            int rfd = events [i].data.fd;
            int index = fd_to_index [rfd];

            int resp_sz = wchans [index]->cread (recvbuf, mb);
            nrecv ++;

            // process (recvbuf)
            vector<char> req = state [index];
            char* request = req.data();
            // processing the response
            MESSAGE_TYPE* m = (MESSAGE_TYPE *) request;
            if (*m == DATA_MSG) {
                //cout << "recvd: " << *(double*)recvbuf << endl;
                hc->update(((datamsg *)request)->person, *(double*)recvbuf);
            } else if (*m == FILE_MSG) {
                //cout << "file msg" << endl;
                filemsg* fm = (filemsg *) request;
                string fname = (char *)(fm + 1);
                //int sz = sizeof (filemsg) + fname.size () + 1;

                string recvfname = "recv/" + fname;

                FILE* fp = fopen (recvfname.c_str(), "r+");
                fseek (fp, fm->offset, SEEK_SET);
                fwrite (recvbuf, 1, fm->length, fp);
                fclose (fp);
            }

            // reuse
            if (!quit_recv) {
                int req_sz = request_buffer->pop(buf, sizeof(buf));
                if (*(MESSAGE_TYPE *) buf == QUIT_MSG) {
                    quit_recv = true;
                } else {
                    wchans[index]->cwrite(buf, req_sz);
                    state[index] = vector<char>(buf, buf + req_sz);
                    nsent++;
                }
            }
        }
    }
}


int main(int argc, char *argv[])
{
    int n = 15000;    //default number of requests per "patient"
    int p = 15;     // number of patients [1,15]
    int w = 200;    //default number of worker threads
    int b = 500; 	// default capacity of the request buffer, you should change this default
	int m = MAX_MESSAGE; 	// default capacity of the message buffer
    srand(time_t(NULL));
    string fname = "10.csv";
    bool filetransfer = false;
    int opt = -1;
    string host, port;
    while ((opt = getopt(argc, argv, "m:n:b:w:p:f:h:r:")) != -1) {
        switch (opt) {
            case 'm':
                m = atoi (optarg);
                cout << "m: " << m << endl;
                break;
            case 'n':
                n = atoi (optarg);
                cout << "n: " << n << endl;
                break;
            case 'p':
                p = atoi (optarg);
                cout << "p: " << p << endl;
                break;
            case 'b':
                b = atoi (optarg);
                cout << "b: " << b << endl;
                break;
            case 'w':
                w = atoi (optarg);
                cout << "w: " << w << endl;
                break;
            case 'f':
                filetransfer = true;
                fname = optarg;
                cout << "File name is: " << fname << endl;
                break;
            case 'h':
                host = optarg;
                cout << "host: " << host << endl;
                break;
            case 'r':
                port = optarg;
                cout << "port: " << port << endl;
                break;
        }
    }

    cout << "Created new channel, connected to server!!!" << endl;
    BoundedBuffer request_buffer (b);
	HistogramCollection hc;

	// making histograms and adding to collection hc
	for (int i = 0; i < p; i++) {
        Histogram* h = new Histogram(10, -2.0, 2.0);
	    hc.add(h);
	}

	TCPRequestChannel** wchans = new TCPRequestChannel* [w];
	for (int i = 0; i < w; ++i) {
	    wchans [i] = new TCPRequestChannel (host, port);
	}


    struct timeval start, end;
    gettimeofday (&start, 0);

    thread patient[p];
    MESSAGE_TYPE q;
    TCPRequestChannel* file_channel = new TCPRequestChannel(host, port);
    if (!filetransfer) {
        cout << "creating patient threads" << endl;
        for (int i = 0; i < p; i++) {
            patient[i] = thread(patient_thread_function, n, i + 1, &request_buffer);
        }
        thread evp (event_polling_thread, w, m, wchans, &request_buffer, &hc);
        for (int i = 0; i < p; i++) {
            patient[i].join();
        }
        q = QUIT_MSG;
        request_buffer.push ((char *) &q, sizeof (q));
        evp.join();
        cout << "Worker threads finished" << endl;
    } else {
        cout << "creating file thread" << endl;
        thread filethread(file_thread_function, fname, &request_buffer, file_channel, m);
        thread evp (event_polling_thread, w, m, wchans, &request_buffer, &hc);
        filethread.join();
        q = QUIT_MSG;
        request_buffer.push ((char *) &q, sizeof (q));
        evp.join();
        cout << "Worker threads finished" << endl;
    }
    gettimeofday (&end, 0);
    // print time diff
    timediff(start, end);

    if (!filetransfer) {
        cout << "Histogram: " << endl;
        hc.print();
    }

	// send quit msg to exit
    cout << "Cleaning memory" << endl;
    for (int i = 0; i < w; ++i) {
        wchans[i]->cwrite(&q, sizeof(MESSAGE_TYPE));
        delete wchans [i];
    }
    delete [] wchans;
    delete file_channel;
    cout << "All Done!!!" << endl;
}
