set(SOURCES DemuxMultiSource.cpp
            DVDDemux.cpp
            DVDDemuxBXA.cpp
            DVDDemuxCC.cpp
            DVDDemuxCDDA.cpp
            DVDDemuxClient.cpp
            DVDDemuxFFmpeg.cpp
            DVDDemuxFFmpegArchive.cpp
            DemuxStreamSSIF.cpp
            DemuxMVC.cpp
            DVDDemuxUtils.cpp
            DVDDemuxVobsub.cpp
            DVDFactoryDemuxer.cpp)

set(HEADERS DemuxMultiSource.h
            DVDDemux.h
            DVDDemuxBXA.h
            DVDDemuxCC.h
            DVDDemuxCDDA.h
            DVDDemuxClient.h
            DVDDemuxFFmpeg.h
            DVDDemuxFFmpegArchive.h
            DemuxStreamSSIF.h
            DemuxMVC.h
            DVDDemuxUtils.h
            DVDDemuxVobsub.h
            DVDFactoryDemuxer.h)

core_add_library(dvddemuxers)
