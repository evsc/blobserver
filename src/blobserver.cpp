/*
 * Copyright (C) 2012 Emmanuel Durand
 *
 * This file is part of blobserver.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * blobserver is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with blobserver.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @blobserver.cpp
 * The main program from the blobserver suite.
 */

#include <chrono>
#include <ctime>
#include <dlfcn.h>
#include <iostream>
#include <iomanip>
#include <limits>
#include <stdio.h>
#include <stdlib.h>

#include "blobserver.h"

#if HAVE_ARAVIS
#include "source_2d_gige.h"
#endif
#include "source_2d_opencv.h"
#include "source_2d_shmdata.h"
#include "source_3d_shmdata.h"

using namespace std;

static gboolean gVersion = FALSE;
static gboolean gHide = FALSE;
static gboolean gVerbose = FALSE;

static int gFramerate = 30;

static gchar* gConfigFile = NULL;
static gchar* gMaskFilename = NULL;
static gboolean gTcp = FALSE;
static gchar* gPort = NULL;

static gboolean gBench = FALSE;
static gboolean gDebug = FALSE;

static GOptionEntry gEntries[] =
{
    {"version", 'v', 0, G_OPTION_ARG_NONE, &gVersion, "Shows version of this software", NULL},
    {"config", 'C', 0, G_OPTION_ARG_STRING, &gConfigFile, "Specify a configuration file to load at startup", NULL},
    {"hide", 'H', 0, G_OPTION_ARG_NONE, &gHide, "Hides the camera window", NULL},
    {"verbose", 'V', 0, G_OPTION_ARG_NONE, &gVerbose, "If set, outputs values to the std::out", NULL},
    {"framerate", 'f', 0, G_OPTION_ARG_INT, &gFramerate, "Specifies the framerate to which blobserver should run (default 30)", NULL},
    {"tcp", 't', 0, G_OPTION_ARG_NONE, &gTcp, "Use TCP instead of UDP for message transmission", NULL},
    {"port", 'p', 0, G_OPTION_ARG_STRING, &gPort, "Specifies TCP port to use for server (default 9002)", NULL},
    {"bench", 'B', 0, G_OPTION_ARG_NONE, &gBench, "Enables printing timings of main loop, for debug purpose", NULL},
    {"debug", 'd', 0, G_OPTION_ARG_NONE, &gDebug, "Enables printing of debug messages", NULL},
    {NULL}
};

shared_ptr<App> App::mInstance(nullptr);
unsigned int App::mCurrentId = 0;

/*****************/
App::App()
{
    mCurrentId = 0;
    mThreadPool.reset(new ThreadPool(4));
#if HAVE_MAPPER
    mMapperDevice = NULL;
#endif
}


/*****************/
App::~App()
{
#if HAVE_MAPPER
    if (mMapperDevice != NULL)
        mdev_free(mMapperDevice);
#endif // HAVE_MAPPER
}

/*****************/
shared_ptr<App> App::getInstance()
{
    if(App::mInstance.get() == nullptr)
        App::mInstance.reset(new App);
    return App::mInstance;
}

/*****************/
void App::stop()
{
    mRun = false;
}

/*****************/
void leave(int sig)
{
    shared_ptr<App> theApp = App::getInstance();
    theApp->stop();
}

/*****************/
int App::init(int argc, char** argv)
{
    (void) signal(SIGINT, leave);

    // Initialize the logger
    g_log_set_handler(NULL, (GLogLevelFlags)(G_LOG_LEVEL_INFO | G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_ERROR | G_LOG_FLAG_FATAL),
                      logHandler, this);
    g_log_set_handler(LOG_BROADCAST, (GLogLevelFlags)(G_LOG_LEVEL_INFO | G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_ERROR | G_LOG_FLAG_FATAL),
                      logHandler, this);

    gPort = (gchar*)"9002";

    // Parse arguments
    int ret = parseArgs(argc, argv);
    if(ret)
        return ret;

    // Register source and actuator classes
    registerClasses();

    // Initialize OSC
    int lNetProto;
    if (gTcp)
        lNetProto = LO_TCP;
    else
        lNetProto = LO_UDP;

    g_log(NULL, G_LOG_LEVEL_INFO, "Cleaning up shared memory in /tmp...");

    GDir* directory;
    GError* error;
    directory = g_dir_open((const gchar*)"/tmp", 0, &error);
    const gchar* filename;
    while ((filename = g_dir_read_name(directory)) != NULL)
    {
        if (strstr((const char*)filename, (const char*)"blobserver") != NULL)
        {
            char buffer[128];
            sprintf(buffer, "/tmp/%s", filename);

            g_log(NULL, G_LOG_LEVEL_INFO, "Removing file %s", buffer);
            g_remove((const gchar*)buffer);
        }
    }
    g_dir_close(directory);

    // Server
    mOscServer = lo_server_thread_new_with_proto(gPort, lNetProto, App::oscError);
    if (mOscServer != NULL)
    {
        lo_server_thread_add_method(mOscServer, "/blobserver/signIn", NULL, App::oscHandlerSignIn, NULL);
        lo_server_thread_add_method(mOscServer, "/blobserver/signOut", NULL, App::oscHandlerSignOut, NULL);
        lo_server_thread_add_method(mOscServer, "/blobserver/changePort", NULL, App::oscHandlerChangePort, NULL);
        lo_server_thread_add_method(mOscServer, "/blobserver/changeIp", NULL, App::oscHandlerChangeIp, NULL);
        lo_server_thread_add_method(mOscServer, "/blobserver/connect", NULL, App::oscHandlerConnect, NULL);
        lo_server_thread_add_method(mOscServer, "/blobserver/disconnect", NULL, App::oscHandlerDisconnect, NULL);
        lo_server_thread_add_method(mOscServer, "/blobserver/setParameter", NULL, App::oscHandlerSetParameter, NULL);
        lo_server_thread_add_method(mOscServer, "/blobserver/getParameter", NULL, App::oscHandlerGetParameter, NULL);
        lo_server_thread_add_method(mOscServer, "/blobserver/actuators", NULL, App::oscHandlerGetActuators, NULL);
        lo_server_thread_add_method(mOscServer, "/blobserver/sources", NULL, App::oscHandlerGetSources, NULL);
        lo_server_thread_add_method(mOscServer, NULL, NULL, App::oscGenericHandler, NULL);
        lo_server_thread_start(mOscServer);
    }
    else
    {
        g_log(NULL, G_LOG_LEVEL_ERROR, "TCP port not available for the OSC server to launch");
        exit(1);
    }

    // Libmapper
#if HAVE_MAPPER
    mMapperDevice = mdev_new(PACKAGE, 9600, 0);
#endif

    // Configuration file needs to be loaded in a thread
    if (gConfigFile != NULL)
    {
        Configurator configurator;
        configurator.loadXML((char*)gConfigFile);
    }

    g_log(NULL, G_LOG_LEVEL_INFO, "Configuration loaded");

    // We need a nap before launching cameras
    timespec nap;
    nap.tv_nsec = 0;
    nap.tv_sec = 1;
    nanosleep(&nap, NULL);

    // Create the thread which will grab from all sources
    // This must be run AFTER loading the configuration, as some params
    // can't be changed after the first grab for some sources
    mRun = true;
    mSourcesThread.reset(new thread(updateSources));

    nanosleep(&nap, NULL);

    return 0;
}

/*****************/
void App::logHandler(const gchar* log_domain, GLogLevelFlags log_level, const gchar* message, gpointer user_data)
{
    App* theApp = static_cast<App*>(user_data);

    if (message == NULL)
        return;

    time_t now = time(NULL);
    char chrNow[100];
    strftime(chrNow, 100, "%T", localtime(&now));

    if (log_domain == NULL)
    {
        switch (log_level)
        {
        case G_LOG_LEVEL_ERROR:
        {
            cout << chrNow << " ";
            cout << "[ERROR] ";
            break;
        }
        case G_LOG_LEVEL_WARNING:
        {
            cout << chrNow << " ";
            cout << "[WARNING] ";
            break;
        }
        case G_LOG_LEVEL_INFO:
        {
            cout << chrNow << " ";
            cout << "[INFO] ";
            break;
        }
        case G_LOG_LEVEL_DEBUG:
        {
            if (!gDebug)
                return;

            cout << chrNow << " ";
            cout << "[DEBUG] ";
            break;
        }
        default:
            break;
        }
        cout << message << endl;
    }
    else if (strcmp(log_domain, LOG_BROADCAST) == 0)
    {
        atom::Message msg;
        char tmpMessage[255];
        strncpy(tmpMessage, message, strlen(message));
        char* token = strtok(tmpMessage, " ");
        while (token != NULL)
        {
            msg.push_back(atom::StringValue::create(token));
            token = strtok(NULL, " ");
        }

        if (log_level == G_LOG_LEVEL_INFO)
        {
            theApp->sendToAllClients("/blobserver/broadcast", msg);
        }
    }
}

/*****************/
int App::parseArgs(int argc, char** argv)
{
    GError *error = NULL;
    GOptionContext* context;

    context = g_option_context_new("- blobserver, detects objects and sends result through OSC");
    g_option_context_add_main_entries(context, gEntries, NULL);
    //g_option_context_add_group(context, gst_init_get_option_group());

    if (!g_option_context_parse(context, &argc, &argv, &error))
    {
        g_log(NULL, G_LOG_LEVEL_ERROR, "Error while parsing options: %s", error->message);
        return 1;
    }

    if (gVersion)
    {
        cout << PACKAGE_TARNAME << " " << PACKAGE_VERSION << endl;
        //g_log(NULL, G_LOG_LEVEL_INFO, "%s %s", PACKAGE_TARNAME, PACKAGE_VERSION);
        return 1;
    }

    return 0;
}

/*****************/
void App::registerClasses()
{
    // Register sources
    mSourceFactory.register_class<Source_2D_OpenCV>(Source_2D_OpenCV::getClassName(),
        Source_2D_OpenCV::getDocumentation());
#if HAVE_ARAVIS
    mSourceFactory.register_class<Source_2D_Gige>(Source_2D_Gige::getClassName(),
        Source_2D_Gige::getDocumentation());
#endif
#if HAVE_SHMDATA
    mSourceFactory.register_class<Source_2D_Shmdata>(Source_2D_Shmdata::getClassName(),
        Source_2D_Shmdata::getDocumentation());
#endif // HAVE_SHMDATA
#if HAVE_PCL && HAVE_SHMDATA
    mSourceFactory.register_class<Source_3D_Shmdata>(Source_3D_Shmdata::getClassName(),
        Source_3D_Shmdata::getDocumentation());
#endif // HAVE_PCL && HAVE_SHMDATA

    loadPlugins();
}

/*************/
void App::loadPlugins()
{

    string prefix = string(LIBDIR) + string("/blobserver-") + string(LIBBLOBSERVER_API_VERSION) + string("/");
    GError* error;
    GDir* dir = g_dir_open(prefix.c_str(), 0, &error);
    if (dir == NULL)
    {
        g_log(NULL, G_LOG_LEVEL_WARNING, "No plugin directory at path %s", prefix.c_str());
        return;
    }

    char* filename = (char*)g_dir_read_name(dir);
    while (filename != NULL)
    {
        string strFilename = string(filename);
        if (strFilename.substr(strFilename.size() - 3, strFilename.size()) == string(".so"))
        {
            //g_log(NULL, G_LOG_LEVEL_DEBUG, "Found lib %s", strFilename.c_str());

            string path = prefix + strFilename;
            void* handler;
            if (gDebug)
                handler = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
            else
                handler = dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);

            if (handler == NULL)
            {
                char* error = dlerror();
                g_log(NULL, G_LOG_LEVEL_WARNING, "%s - %s", __FUNCTION__, error);
            }
            else
            {
                void* registerToFactory = dlsym(handler, "registerToFactory");
                if (registerToFactory == NULL)
                    g_log(NULL, G_LOG_LEVEL_WARNING, "%s - %s", __FUNCTION__, dlerror());
                else
                {
                    typedef void (*func)(factory::AbstractFactory<Actuator, string, string, string>&);
                    ((func)registerToFactory)(mActuatorFactory);
                }
            }
        }
        filename = (char*)g_dir_read_name(dir);
    }

    g_dir_close(dir);
}

/*************/
void timeSince(unsigned long long timestamp, std::string stage)
{
    auto now = chrono::high_resolution_clock::now();
    unsigned long long currentTime = chrono::duration_cast<chrono::microseconds>(now.time_since_epoch()).count();
    g_log(NULL, G_LOG_LEVEL_INFO, "%s - %lli ms", stage.c_str(), ((long long)currentTime - (long long)timestamp)/1000);
}

/*****************/
int App::loop()
{
    int frameNbr = 0;

    bool lShowCamera = !gHide;
    int lSourceNumber = 0;

    unsigned long long usecPeriod = 1e6 / (long long)gFramerate;

    mutex lMutex;

    while(mRun)
    {
        unsigned long long chronoStart;
        chronoStart = chrono::duration_cast<chrono::microseconds>(chrono::high_resolution_clock::now().time_since_epoch()).count();

        vector< Capture_Ptr > lBuffers;
        vector<string> lBufferNames;

        // First buffer is a black screen. No special reason, except we need
        // a first buffer
        lBuffers.push_back(Capture_2D_Mat_Ptr(new Capture_2D_Mat(cv::Mat::zeros(480, 640, CV_8UC3))));
        lBufferNames.push_back(string("This is Blobserver"));

        // Retrieve the capture from all the sources
        {
            // First we grab, then we retrieve all frames
            // This way, sync between frames is better
            for_each (mSources.begin(), mSources.end(), [&] (shared_ptr<Source> source)
            {
                lBuffers.push_back(source->retrieveFrame());

                atom::Message msg;
                msg.push_back(atom::StringValue::create("id"));
                msg = source->getParameter(msg);
                string id = atom::toString(msg[1]);
                lBufferNames.push_back(source->getName() + string(" ") + id);
            } );
        }

        if (gBench)
            timeSince(chronoStart, string("Benchmark - Retrieve frames"));

        // Go through the flows
        {
            lock_guard<mutex> lock(mFlowMutex);
            vector<shared_ptr<thread> > threads;
            threads.resize(mFlows.size());

            // Update all sources for all flows
            for (int index = 0; index < mFlows.size(); ++index)
            {
                Flow* flow = &mFlows[index];
                if (flow->run == false)
                    continue;

                // Apply the actuator on these frames
                mThreadPool->enqueue([=, &lMutex] ()
                {
                    // Retrieve the frames from all sources in this flow
                    // There is no risk for sources to disappear here, so no
                    // need for a mutex (they are freed earlier)
                    vector< Capture_Ptr > frames;
                    {
                        for (int i = 0; i < flow->sources.size(); ++i)
                        {
                            lock_guard<mutex> lock(lMutex);
                            frames.push_back(flow->sources[i]->retrieveFrame());
                        }
                    }
                    flow->actuator->detect(frames);
                } );
            }
            // Wait for all actuators to finish
            mThreadPool->waitAllThreads(); 

            if (gBench)
                timeSince(chronoStart, string("Benchmark - Update actuators"));

            for_each (mFlows.begin(), mFlows.end(), [&] (Flow& flow)
            {
                if (flow.run == false)
                    return;

                // Get the message resulting from the detection
                atom::Message message;
                message = flow.actuator->getLastMessage();

                vector<Capture_Ptr> output = flow.actuator->getOutput();
                for_each (output.begin(), output.end(), [&] (Capture_Ptr& img)
                {
                    lBuffers.push_back(img);
                    lBufferNames.push_back(flow.actuator->getName());
                });

                if (flow.actuator->getName() == "Actuator_Hog") {
                    // check last item on message
                    int mvmnt = atom::toInt(message[7]);
                    atom::Message msg;
                    msg.push_back("enableRecording");
                    msg.push_back(mvmnt);
                    flow->sources[0]->setParameter(msg);
                    cout << "Loop() \tActuator_Hog \t" << "enableRecording \t" << mvmnt << endl;
                }

#if HAVE_SHMDATA
                if (flow.sink.size() < output.size())
                    for (int i = flow.sink.size(); i < output.size(); ++i)
                    {
                        char shmFile[128];
                        sprintf(shmFile, "/tmp/blobserver_%i_%s_%i", flow.id, flow.actuator->getOscPath().c_str(), i);
                        shared_ptr<Shm> shm;
                        shm.reset(new ShmAuto(shmFile));
                        flow.sink.push_back(shm);
                    }
                        
                for (int i = 0; i < output.size(); ++i)
                    flow.sink[i]->setCapture(output[i]);
#endif

                // Send OSC messages
                // Beginning of the frame
                lo_send(flow.client->get(), "/blobserver/startFrame", "ii", frameNbr, flow.id);

                int nbr, size;
                if (message.size() < 2)
                {
                    nbr = 0;
                    size = 0;
                }
                else
                {
                    nbr = atom::toInt(message[0]);
                    size = atom::toInt(message[1]);
                }

                for (int i = 0; i < nbr; ++i)
                {
                    atom::Message msg;
                    for (int j = 0; j < size; ++j)
                        msg.push_back(message[i * size + 2 + j]);
                    
                    lo_message oscMsg = lo_message_new();
                    atom::message_build_to_lo_message(msg, oscMsg);
                    lo_send_message(flow.client->get(), (string("/blobserver/") + flow.actuator->getOscPath()).c_str(), oscMsg);
                    free(oscMsg);
                }

#if HAVE_MAPPER
                if (flow.mapperSignal.size() < nbr)
                {
                    for (int index = flow.mapperSignal.size(); index < nbr; ++index)
                    {
                        int intSize = 0;
                        for (int i = 0; i < size; ++i)
                        {
                            char type = message[i + 2]->getTypeTag();
                            if (type != atom::IntValue::TYPE_TAG && type != atom::FloatValue::TYPE_TAG)
                                continue;
                            intSize++;
                        }

                        string path = to_string(flow.id) + string("_") + flow.actuator->getOscPath() + string("_") + to_string(index);
                        mapper_signal signal = mdev_add_output(mMapperDevice, path.c_str(), intSize, 'f', 0, 0, 0);
                        flow.mapperSignal.push_back(signal);
                    }
                }

                if (message.size() > 2)
                for (int index = 0; index < nbr; ++index)
                {
                    vector<float> values;
                    for (int i = 0; i < size; ++i)
                    {
                        char type = message[i + index*size + 2]->getTypeTag();
                        if (type != atom::IntValue::TYPE_TAG && type != atom::FloatValue::TYPE_TAG)
                            continue;
                        values.push_back(atom::toFloat(message[i + index*size + 2]));
                    }
                    msig_update(flow.mapperSignal[index], values.data(), values.size(), MAPPER_NOW);
                }
#endif

                // End of the frame
                lo_send(flow.client->get(), "/blobserver/endFrame", "ii", frameNbr, flow.id);
            } );

#if HAVE_MAPPER
            mdev_poll(mMapperDevice, 0);
#endif

            if (gBench)
                timeSince(chronoStart, string("Benchmark - Update buffers"));
        }

        if (lShowCamera)
        {
            // Check if the current source number is still available
            if (lSourceNumber >= lBuffers.size())
                lSourceNumber = 0;

            Capture_2D_Mat_Ptr img = dynamic_pointer_cast<Capture_2D_Mat>(lBuffers[lSourceNumber]);
            if (img.get() != NULL)
            {
                cv::Mat displayMat = img->get().clone();
                if (displayMat.depth() == CV_32F)
                {
                    float maxValue = 0.f;
                    for (int x = 0; x < displayMat.cols; ++x)
                        for (int y = 0; y < displayMat.rows; ++y)
                            maxValue = max(maxValue, displayMat.at<cv::Vec3f>(y, x)[0]);
            
                    cv::Mat buffer = cv::Mat::zeros(displayMat.size(), CV_8UC3);
                    displayMat /= maxValue;
                    cv::pow(displayMat, 1.0 / 1.8, displayMat);
                    displayMat *= 2.0 * 255.f;
                    displayMat.convertTo(buffer, CV_8UC3);
                    displayMat = buffer;
                }
                cv::putText(displayMat, lBufferNames[lSourceNumber].c_str(), cv::Point(10, 30),
                    cv::FONT_HERSHEY_COMPLEX, 1.0, cv::Scalar::all(0.0), 3.0);
                cv::putText(displayMat, lBufferNames[lSourceNumber].c_str(), cv::Point(10, 30),
                    cv::FONT_HERSHEY_COMPLEX, 1.0, cv::Scalar::all(255.0));
                cv::imshow("blobserver", displayMat);
            }

            char lKey = cv::waitKey(1);
            if(lKey == 27) // Escape
                mRun = false;
            if(lKey == 'w')
            {
                lSourceNumber = (lSourceNumber+1)%lBuffers.size();
                g_log(NULL, G_LOG_LEVEL_INFO, "Buffer displayed: %s", lBufferNames[lSourceNumber].c_str());
            }
        }

        unsigned long long chronoEnd = chrono::duration_cast<chrono::microseconds>(chrono::high_resolution_clock::now().time_since_epoch()).count();
        unsigned long long chronoElapsed = chronoEnd - chronoStart;
        
        timespec nap;
        nap.tv_sec = 0;
        if (chronoElapsed < usecPeriod)
            nap.tv_nsec = (usecPeriod - chronoElapsed) * 1e3;
        else
            nap.tv_nsec = 0;

        nanosleep(&nap, NULL);

        if (gBench)
            timeSince(chronoStart, string("Benchmark - Total frame time"));

        frameNbr++;
    }

    g_log(NULL, G_LOG_LEVEL_INFO, "Leaving...");
    mSourcesThread->join();

    return 0;
}

/*****************/
void App::updateSources()
{
    shared_ptr<App> theApp = App::getInstance();

    unsigned long long usecPeriod = 1e6 / (long long)gFramerate;

    while(theApp->mRun)
    {
        unsigned long long chronoStart = chrono::duration_cast<chrono::milliseconds>(chrono::high_resolution_clock::now().time_since_epoch()).count();

        {
            lock_guard<mutex> lock(theApp->mSourceMutex);
            
            vector<shared_ptr<Source>>::iterator iter;
            // First we grab, then we retrieve all frames
            // This way, sync between frames is better
            for (iter = theApp->mSources.begin(); iter != theApp->mSources.end(); ++iter)
            {
                shared_ptr<Source> source = (*iter);
                source->grabFrame();
            
                // We also check if this source is still used
                if (source.use_count() == 2) // 2, because this ptr and the one in the vector
                {
                    g_log(NULL, G_LOG_LEVEL_INFO, "%s - Source %s is no longer used. Disconnecting.", __FUNCTION__, source->getName().c_str());
                    theApp->mSources.erase(iter);
                    --iter;
                }
            }
        }

        unsigned long long chronoEnd = chrono::duration_cast<chrono::milliseconds>(chrono::high_resolution_clock::now().time_since_epoch()).count();
        unsigned long long chronoElapsed = (chronoEnd - chronoStart) * 1e3;

        timespec nap;
        nap.tv_sec = 0;
        if (chronoElapsed < usecPeriod)
            nap.tv_nsec = (usecPeriod - chronoElapsed) * 1e3;
        else
            nap.tv_nsec = 0;

        nanosleep(&nap, NULL);
    }
}

/*****************/
void App::oscError(int num, const char* msg, const char* path)
{
    g_log(NULL, G_LOG_LEVEL_WARNING, "liblo server error %i", num);
}

/*****************/
int App::oscGenericHandler(const char* path, const char* types, lo_arg** argv, int argc, void* data, void* user_data)
{
    if(gVerbose)
    {
        g_log(NULL, G_LOG_LEVEL_WARNING, "%s - Unhandled message received:", __FUNCTION__);

        for(int i = 0; i < argc; ++i)
        {
            lo_arg_pp((lo_type)(types[i]), argv[i]);
        }

        cout << endl;
    }

    return 1;
}

/*****************/
int App::oscHandlerSignIn(const char* path, const char* types, lo_arg** argv, int argc, void* data, void* user_data)
{
    shared_ptr<App> theApp = App::getInstance();

    atom::Message message;
    atom::message_build_from_lo_args(message, types, argv, argc);

    if (message.size() < 2)
    {
        g_log(NULL, G_LOG_LEVEL_WARNING, "%s - Wrong number of arguments received.", __FUNCTION__);
        return 1;
    }

    char port[8];
    string addressStr;
    try
    {
        addressStr = atom::toString(message[0]);

        int portNbr = atom::toInt(message[1]);
        sprintf(port, "%i", portNbr);
    }
    catch (atom::BadTypeTagError exception)
    {
        return 1;
    }

    // Check wether this address is already signed in
    if (theApp->mClients.find(addressStr) == theApp->mClients.end())
    {
        shared_ptr<OscClient> address(new OscClient(lo_address_new(addressStr.c_str(), port)));
        int error = lo_address_errno(address->get());
        if (error != 0)
        {
            g_log(NULL, G_LOG_LEVEL_WARNING, "%s - Received address wrongly formated.", __FUNCTION__);
            return 0;
        }

        lock_guard<mutex> lock(theApp->mFlowMutex);
        theApp->mClients[addressStr] = address;
        lo_send(address->get(), "/blobserver/signIn", "s", "Sucessfully signed in to the blobserver.");
    }
    // If already connected, we send a message to say so
    else
    {
        shared_ptr<OscClient> address(new OscClient(lo_address_new(addressStr.c_str(), port)));
        int error = lo_address_errno(address->get());
        if (error != 0)
        {
            g_log(NULL, G_LOG_LEVEL_WARNING, "%s - Received address wrongly formated.", __FUNCTION__);
            return 0;
        }

        lo_send(address->get(), "/blobserver/signIn", "s", "This address seem to be already connected on another port.");
    }

    return 0;
}

/*****************/
int App::oscHandlerSignOut(const char* path, const char* types, lo_arg** argv, int argc, void* data, void* user_data)
{
    shared_ptr<App> theApp = App::getInstance();

    atom::Message message;
    atom::message_build_from_lo_args(message, types, argv, argc);

    if (message.size() != 1)
    {
        g_log(NULL, G_LOG_LEVEL_WARNING, "%s - Wrong number of arguments received.", __FUNCTION__);
        return 1;
    }

    string addressStr;
    try
    {
        addressStr = atom::toString(message[0]);
    }
    catch (atom::BadTypeTagError exception)
    {
        return 1;
    }

    if (theApp->mClients.find(addressStr) != theApp->mClients.end())
    {
        // Disconnect all flows related to this address
        oscHandlerDisconnect((const char*)"", types, argv, argc, data, user_data);

        // Remove the client from the list
        lock_guard<mutex> lock(theApp->mFlowMutex);
        theApp->mClients.erase(addressStr);
    }

    return 0;
}

/*****************/
int App::oscHandlerChangePort(const char* path, const char* types, lo_arg** argv, int argc, void* data, void* user_data)
{
    shared_ptr<App> theApp = App::getInstance();
    
    atom::Message message;
    atom::message_build_from_lo_args(message, types, argv, argc);

    string addressStr;
    try
    {
        addressStr = atom::toString(message[0]);
    }
    catch (atom::BadTypeTagError exception)
    {
        return 0;
    }

    shared_ptr<OscClient> address;
    if (theApp->mClients.find(addressStr) != theApp->mClients.end())
    {
        address = theApp->mClients[addressStr];
    }
    else
    {
        return 0;
    }

    if (message.size() != 2)
    {
        lo_send(address->get(), "/blobserver/changePort", "s", "Wrong number of arguments.");
    }

    char port[8];
    try
    {
        int portNbr = atom::toInt(message[1]);
        sprintf(port, "%i", portNbr);
    }
    catch (atom::BadTypeTagError exception)
    {
        return 1;
    }

    // Change the port number
    {
        lock_guard<mutex> lock(theApp->mFlowMutex);
        theApp->mClients[addressStr]->replace(lo_address_new(addressStr.c_str(), port));
    }

    return 0;
}

/*****************/
int App::oscHandlerChangeIp(const char* path, const char* types, lo_arg** argv, int argc, void* data, void* user_data)
{
    shared_ptr<App> theApp = App::getInstance();
    
    atom::Message message;
    atom::message_build_from_lo_args(message, types, argv, argc);

    string addressStr;
    try
    {
        addressStr = atom::toString(message[0]);
    }
    catch (atom::BadTypeTagError exception)
    {
        return 0;
    }

    shared_ptr<OscClient> address;
    if (theApp->mClients.find(addressStr) != theApp->mClients.end())
    {
        address = theApp->mClients[addressStr];
    }
    else
    {
        return 0;
    }

    if (message.size() != 2)
    {
        lo_send(address->get(), "/blobserver/changeIp", "s", "Wrong number of arguments.");
    }

    string newAddress;
    char port[8];
    try
    {
        newAddress = atom::toString(message[1]);
        int portNbr = atom::toInt(message[2]);
        sprintf(port, "%i", portNbr);
    }
    catch (atom::BadTypeTagError exception)
    {
        return 1;
    }

    // Change the port number
    {
        lock_guard<mutex> lock(theApp->mFlowMutex);
        theApp->mClients[addressStr]->replace(lo_address_new(newAddress.c_str(), port));
    }

    return 0;
}

/*****************/
int App::oscHandlerConnect(const char* path, const char* types, lo_arg** argv, int argc, void* data, void* user_data)
{
    shared_ptr<App> theApp = App::getInstance();

    // Messge must be : ip / port / actuator / source0 / subsource0 / source1 / ...
    atom::Message message;
    atom::message_build_from_lo_args(message, types, argv, argc);

    //char port[8];
    string addressStr;
    try
    {
        addressStr = atom::toString(message[0]);
    }
    catch (atom::BadTypeTagError exception)
    {
        return 1;
    }
    
    // Check if the client is signed in
    shared_ptr<OscClient> address;
    if (theApp->mClients.find(addressStr) != theApp->mClients.end())
    {
        address = theApp->mClients[addressStr];
    }
    else
    {
        return 0;
    }

    if (message.size() < 4)
    {
        lo_send(address->get(), "/blobserver/connect", "s", "Too few arguments");
        return 1; 
    }

    // Check arguments
    // First argument is the chosen actuator, next ones are sources
    string actuatorName;
    try
    {
        actuatorName = atom::toString(message[1]);
    }
    catch (atom::BadTypeTagError typeError)
    {
        lo_send(address->get(), "/blobserver/connect", "s", "Expected a actuator type at position 2");
        return 1;
    }

    // Create the specified actuator
    shared_ptr<Actuator> actuator;
    if (theApp->mActuatorFactory.key_exists(actuatorName))
        actuator = theApp->mActuatorFactory.create(actuatorName);
    else
    {
        lo_send(address->get(), "/blobserver/connect", "s", "Actuator type not recognized");
        return 1;
    }

    // Check how many cameras we need for it
    unsigned int sourceNbr = actuator->getSourceNbr();
    
    // Allocate all the sources
    vector<shared_ptr<Source>> sources;
    atom::Message::const_iterator iter;
    for (iter = message.begin()+2; iter != message.end(); iter+=2)
    {
        if (iter+1 == message.end())
        {
            lo_send(address->get(), "/blobserver/connect", "s", "Missing sub-source number");
            return 1;
        }

        string sourceName;
        string sourceID;
        try
        {
            sourceName = atom::toString(*iter);
            sourceID = atom::toString(*(iter+1));
        }
        catch (atom::BadTypeTagError typeError)
        {
            lo_send(address->get(), "/blobserver/connect", "s", "Expected integer as a sub-source number");
            return 1;
        }

        // Check if this source is not already connected
        bool alreadyConnected = false;
        vector<shared_ptr<Source>>::const_iterator iterSource;
        for (iterSource = theApp->mSources.begin(); iterSource != theApp->mSources.end(); ++iterSource)
        {
            if (iterSource->get()->getName() == sourceName && iterSource->get()->getSubsourceNbr() == sourceID)
            {
                sources.push_back(*iterSource);
                alreadyConnected = true;
            }
        }

        if (!alreadyConnected)
        {
            shared_ptr<Source> source;
            if (theApp->mSourceFactory.key_exists(sourceName))
                source = theApp->mSourceFactory.create(sourceName, sourceID);
            else
            {
                string error = "Unable to create source ";
                error += sourceName;
                lo_send(address->get(), "/blobserver/connect", "s", error.c_str());
                return 1;
            }
            
            if (!source->connect())
            {
                string error = "Unable to connect to source ";
                error += sourceName;
                lo_send(address->get(), "/blobserver/connect", "s", error.c_str());
                return 1;
            }

            sources.push_back(source);
        }
    }

    // If enough sources have been specified
    if (sources.size() >= sourceNbr)
    {
        lock_guard<mutex> lock(theApp->mFlowMutex);
        lock_guard<mutex> lockToo(theApp->mSourceMutex);

        // We can create the flow!
        Flow flow;
        
        flow.actuator = actuator;
        flow.client = address;
        flow.id = theApp->getValidId();
        flow.run = false;

        vector<shared_ptr<Source>>::const_iterator source;
        for (source = sources.begin(); source != sources.end(); ++source)
        {
            flow.sources.push_back(*source);

            // Add the sources to the mSources vector
            // (if they are not already there)
            bool isInSources = false;
            vector<shared_ptr<Source>>::const_iterator iter;
            for (iter = theApp->mSources.begin(); iter != theApp->mSources.end(); ++iter)
            {
                if (iter->get()->getName() == source->get()->getName() && iter->get()->getSubsourceNbr() == source->get()->getSubsourceNbr())
                    isInSources = true;
            }
            if (!isInSources)
                theApp->mSources.push_back(*source);

            // Adds a weak ptr to sources to the actuator, for it to control them
            actuator->addSource(*source);
        }

        theApp->mFlows.push_back(flow);

        // Tell the client that he is connected, and give him the flow id
        lo_send(address->get(), "/blobserver/connect", "si", "Connected", (int)flow.id);
    }
    else
    {
        lo_send(address->get(), "/blobserver/connect", "s", "The specified actuator needs more sources");
        return 1;
    }
    
    return 0;
}

/*****************/
int App::oscHandlerDisconnect(const char* path, const char* types, lo_arg** argv, int argc, void* data, void* user_data)
{
    shared_ptr<App> theApp = App::getInstance();
    
    atom::Message message;
    atom::message_build_from_lo_args(message, types, argv, argc);

    string addressStr;
    try
    {
        addressStr = atom::toString(message[0]);
    }
    catch (atom::BadTypeTagError exception)
    {
        return 0;
    }

    shared_ptr<OscClient> address;
    if (theApp->mClients.find(addressStr) != theApp->mClients.end())
    {
        address = theApp->mClients[addressStr];
    }
    else
    {
        return 0;
    }

    if (message.size() != 1 && message.size() != 2)
    {
        lo_send(address->get(), "/blobserver/disconnect", "s", "Wrong number of arguments.");
        return 1;
    }
    
    bool all = false;
    int actuatorId;
    if (message.size() == 1)
        all = true;
    else
        actuatorId = atom::toInt(message[1]);

    // Delete flows related to this address, according to the parameter
    lock_guard<mutex> lock(theApp->mFlowMutex);
    vector<Flow>::iterator flow;
    for (flow = theApp->mFlows.begin(); flow != theApp->mFlows.end();)
    {
        if (string(lo_address_get_url(flow->client->get())) == string(lo_address_get_url(address->get())))
        {
            if (all == true || actuatorId == flow->id)
            {
                lo_send(flow->client->get(), "/blobserver/disconnect", "s", "Disconnected");
                theApp->mFlows.erase(flow);
                g_log(NULL, G_LOG_LEVEL_INFO, "Connection from address %s closed.", addressStr.c_str());
            }
            else
            {
                flow++;
            }
        }
        else
        {
            flow++;
        }
    }

    return 0;
}

/*****************/
int App::oscHandlerSetParameter(const char* path, const char* types, lo_arg** argv, int argc, void* data, void* user_data)
{
    shared_ptr<App> theApp = App::getInstance();    

    atom::Message message;
    atom::message_build_from_lo_args(message, types, argv, argc);
        
    string addressStr;
    try
    {
        addressStr = atom::toString(message[0]);
    }
    catch (atom::BadTypeTagError exception)
    {
        return 1;
    }

    // Check if the client is signed in
    shared_ptr<OscClient> address;
    if (theApp->mClients.find(addressStr) != theApp->mClients.end())
    {
        address = theApp->mClients[addressStr];
    }
    else
    {
        return 0;
    }

    // Message must contain ip address, flow id, target (actuator or src), src number if applicable, parameter and value
    // or just ip address, flow id, and start/stop
    if (message.size() < 3)
    {
        lo_send(address->get(), "/blobserver/setParameter", "s", "Wrong number of arguments");
        return 1;
    }

    // Find the flow
    int result = 0;

    unsigned int flowId = (unsigned int)(atom::toInt(message[1]));
    vector<Flow>::iterator flow;
    for (flow = theApp->mFlows.begin(); flow != theApp->mFlows.end(); ++flow)
    {
        if (flow->id == flowId)
        {
            lock_guard<mutex> lock(theApp->mFlowMutex);

            // If the parameter is for the actuator
            if (atom::toString(message[2]) == "Actuator")
            {
                if (message.size() < 5)
                {
                    lo_send(flow->client->get(), "/blobserver/setParameter", "s", "Wrong number of arguments");
                    result = 1;
                }
                else
                {
                    atom::Message msg;
                    for (int i = 3; i < message.size(); ++i)
                        msg.push_back(message[i]);
                    flow->actuator->setParameter(msg);
                }
            }
            // If the parameter is for one of the sources
            else if (atom::toString(message[2]) == "Source")
            {
                if (message.size() < 6)
                {
                    lo_send(flow->client->get(), "/blobserver/setParameter", "s", "Wrong number of arguments");
                    result = 1;
                }
                else
                {
                    int srcNbr = atom::toInt(message[3]);
                    if (srcNbr >= flow->sources.size())
                    {
                        lo_send(flow->client->get(), "/blobserver/setParameter", "s", "Wrong source index");
                        result = 1;
                    }
                    else
                    {
                        atom::Message msg;
                        for (int i = 4; i < message.size(); ++i)
                            msg.push_back(message[i]);
                        flow->sources[srcNbr]->setParameter(msg);
                    }
                }
            }
            else if (atom::toString(message[2]) == "Start")
            {
                flow->run = true;
            }
            else if (atom::toString(message[2]) == "Stop")
            {
                flow->run = false;
            }
        }
    }

    return result;
}

/*****************/
int App::oscHandlerGetParameter(const char* path, const char* types, lo_arg** argv, int argc, void* data, void* user_data)
{
    shared_ptr<App> theApp = App::getInstance();

    atom::Message message;
    atom::message_build_from_lo_args(message, types, argv, argc);

    string addressStr;
    try
    {
        addressStr = atom::toString(message[0]);
    }
    catch (atom::BadTypeTagError exception)
    {
        return 0;
    }

    shared_ptr<OscClient> address;
    if (theApp->mClients.find(addressStr) != theApp->mClients.end())
    {
        address = theApp->mClients[addressStr];
    }
    else
    {
        return 0;
    }

    if (message.size() < 4)
    {
        lo_send(address->get(), "/blobserver/getParameter", "s", "Wrong number of arguments");
        return 1;
    }

    unsigned int flowId;
    string entity;

    try
    {
        flowId = (unsigned int)(atom::toInt(message[1]));
        entity = atom::toString(message[2]);
    }
    catch (atom::BadTypeTagError exception)
    {
        return 1;
    }

    // Go through the flows
    int result = 0;
    for_each (theApp->mFlows.begin(), theApp->mFlows.end(), [&] (Flow flow)
    {
        if (flow.id == flowId)
        {
            lock_guard<mutex> lock(theApp->mFlowMutex);

            // If the parameter is for the actuator
            if (entity == "Actuator")
            {
                atom::Message msg;
                msg.push_back(message[3]);
                msg = flow.actuator->getParameter(msg);

                lo_message oscMsg = lo_message_new();
                atom::message_build_to_lo_message(msg, oscMsg);
                lo_send_message(flow.client->get(), "/blobserver/getParameter", oscMsg);
            }
            // If the parameter is for the sources
            else if (entity == "Sources")
            {
                if (message.size() < 5)
                {
                    lo_send(flow.client->get(), "/blobserver/getParameter", "s", "Wrong number of arguments");
                    result = 1;
                }
                else
                {
                    int srcNbr;
                    try
                    {
                        srcNbr = atom::toInt(message[3]);
                    }
                    catch (...)
                    {
                        return 1;
                    }

                    if (srcNbr >= flow.sources.size())
                    {
                        result = 1;
                    }
                    else
                    {
                        atom::Message msg;
                        msg.push_back(message[4]);
                        msg = flow.sources[srcNbr]->getParameter(msg);

                        lo_message oscMsg = lo_message_new();
                        atom::message_build_to_lo_message(msg, oscMsg);
                        lo_send_message(flow.client->get(), "/blobserver/getParameter", oscMsg);
                    }
                }
            }
        }
    } );

    return result;
}

/*****************/
int App::oscHandlerGetActuators(const char* path, const char* types, lo_arg** argv, int argc, void* data, void* user_data)
{
    shared_ptr<App> theApp = App::getInstance();

    atom::Message message;
    atom::message_build_from_lo_args(message, types, argv, argc);

    if (message.size() < 1)
        return 1;

    string addressStr;
    try
    {
        addressStr = atom::toString(message[0]);
    }
    catch (atom::BadTypeTagError exception)
    {
        return 0;
    }

    shared_ptr<OscClient> address;
    if (theApp->mClients.find(addressStr) != theApp->mClients.end())
    {
        address = theApp->mClients[addressStr];
    }
    else
    {
        return 0;
    }

    // Get all the available actuators
    vector<string> keys = theApp->mActuatorFactory.get_keys();

    atom::Message outMessage;
    for_each (keys.begin(), keys.end(), [&] (string key)
    {
        outMessage.push_back(atom::StringValue::create(key.c_str()));
    } );

    lo_message oscMsg = lo_message_new();
    atom::message_build_to_lo_message(outMessage, oscMsg);

    lo_send_message(address->get(), "/blobserver/actuators", oscMsg);
}

/*****************/
int App::oscHandlerGetSources(const char* path, const char* types, lo_arg** argv, int argc, void* data, void* user_data)
{
    shared_ptr<App> theApp = App::getInstance();

    atom::Message message;
    atom::message_build_from_lo_args(message, types, argv, argc);

    string addressStr;
    try
    {
        addressStr = atom::toString(message[0]);
    }
    catch (atom::BadTypeTagError exception)
    {
        return 0;
    }

    shared_ptr<OscClient> address;
    if (theApp->mClients.find(addressStr) != theApp->mClients.end())
    {
        address = theApp->mClients[addressStr];
    }
    else
    {
        return 0;
    }
    
    // If we have another parameter, it means we want to get availables subsources
    atom::Message outMessage;
    if (message.size() > 1)
    {
        string sourceName;
        try
        {
            sourceName = atom::toString(message[1]);
        }
        catch (...)
        {
            return 1;
        }

        // We try to create the named source
        shared_ptr<Source> source;
        if (theApp->mSourceFactory.key_exists(sourceName))
            source = theApp->mSourceFactory.create(sourceName, std::string());
        else
            return 1;

        // Ask the source for all the available subsources
        outMessage = source->getSubsources();
    }
    else
    {
        // Get all the available sources
        vector<string> keys = theApp->mSourceFactory.get_keys();

        for_each (keys.begin(), keys.end(), [&] (string key)
        {
            outMessage.push_back(atom::StringValue::create(key.c_str()));
        } );
    }

    lo_message oscMsg = lo_message_new();
    atom::message_build_to_lo_message(outMessage, oscMsg);
    lo_send_message(address->get(), "/blobserver/sources", oscMsg);
}

/*************/
void App::sendToAllClients(const char* path, atom::Message& message)
{
    for_each (mFlows.begin(), mFlows.end(), [&] (Flow flow)
    {
        // Currently, only sends OSC messages, but will change when more
        // connection types are supported
        lo_message oscMsg = lo_message_new();
        atom::message_build_to_lo_message(message, oscMsg);
        lo_send_message(flow.client->get(), path, oscMsg);
    });
}

/*************/
int main(int argc, char** argv)
{
    shared_ptr<App> theApp = App::getInstance();
    int ret;

    ret = theApp->init(argc, argv);
    if(ret != 0)
        return ret;

    ret = theApp->loop();
    return ret;
}
