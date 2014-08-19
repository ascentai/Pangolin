/* This file is part of the Pangolin Project.
 * http://github.com/stevenlovegrove/Pangolin
 *
 * Copyright (c) 2014 Steven Lovegrove
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <pangolin/log/packetstream.h>

#include <iostream>
#include <string>
#include <stdio.h>
#include <time.h>

namespace pangolin
{

const char* json_hdr_type         = "_type_";
const char* json_hdr_uri         = "_uri_";
const char* json_hdr_header      = "header";
const char* json_hdr_typed_aux   = "typed_aux";
const char* json_hdr_typed_frame = "typed_frame";

//////////////////////////////////////////////////////////////////////////
// PacketStreamWriter
//////////////////////////////////////////////////////////////////////////

PacketStreamWriter::PacketStreamWriter(const std::string& filename, unsigned int buffer_size_bytes )
    : buffer(filename, buffer_size_bytes), writer(&buffer), bytes_written(0)
{
    // Start of file magic
    WriteTag(TAG_PANGO_MAGIC);
    WritePangoHeader();
}

PacketStreamWriter::~PacketStreamWriter()
{
    WriteStats();
}

PacketStreamSourceId PacketStreamWriter::AddSource(
    const std::string& type,
    const std::string& uri,
    const std::string& json_frame,
    const std::string& json_header,
    const std::string& json_aux_types
) {
    const std::string ns = type+"::";

    std::string err;
    picojson::value header;
    picojson::value typed_aux;
    picojson::value typed_frame;

    picojson::parse(header,json_header.begin(), json_header.end(), &err);
    if(!err.empty()) throw std::runtime_error("Frame header parse error: " + err);

    picojson::parse(typed_aux,json_aux_types.begin(), json_aux_types.end(), &err);
    if(!err.empty()) throw std::runtime_error("Frame types parse error: " + err);

    picojson::parse(typed_frame,json_frame.begin(), json_frame.end(), &err);
    if(!err.empty()) throw std::runtime_error("Frame definition parse error: " + err);

    if( typed_aux.is<picojson::object>() ) {
        typemap.AddTypes(ns, typed_aux.get<picojson::object>() );
        PacketStreamTypeId frame_id = typemap.CreateOrGetType(ns, typed_frame);
        typemap.AddAlias(ns + PANGO_FRAME, frame_id);

        picojson::value json_src(picojson::object_type,false);
        json_src.get<picojson::object>()[json_hdr_type] = picojson::value(type);
        json_src.get<picojson::object>()[json_hdr_uri] = picojson::value(uri);
        json_src.get<picojson::object>()[json_hdr_header] = header;
        json_src.get<picojson::object>()[json_hdr_typed_aux] = typed_aux;
        json_src.get<picojson::object>()[json_hdr_typed_frame] = typed_frame;

        WriteTag(TAG_ADD_SOURCE);
        json_src.serialize(std::ostream_iterator<char>(writer), true);

        const PacketStreamTypeId frame_type = typemap.GetTypeId(ns + PANGO_FRAME);
        const size_t src_id = sources.size();
        sources.push_back( {type,uri,header,frame_type} );
        return src_id;
    }else{
        throw std::runtime_error("Frame definition must be JSON object.");
    }
}

void PacketStreamWriter::WriteSourceFrame(PacketStreamSourceId src, char* data, size_t n)
{
    // Write SOURCE_FRAME tag and source id
    WriteTag(TAG_SRC_FRAME);
    WriteCompressedUnsignedInt(src);

    // Write frame size if dynamic so it can be skipped over easily
    const PacketStreamType& frametype = typemap.GetType(sources[src].frametype);
    if(!frametype.IsFixedSize()) {
        WriteCompressedUnsignedInt(n);
    }else if(frametype.size_bytes != n) {
        throw std::runtime_error("Attempting to write frame of wrong size");
    }

    // Write data
    writer.write(data, n);
    bytes_written += n;
}

const std::string CurrentTimeStr() {
    time_t time_now = time(0);
    struct tm time_struct = *localtime(&time_now);
    char buffer[80];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %X", &time_struct);
    return buffer;
}

void PacketStreamWriter::WritePangoHeader()
{
    // Write Header
    picojson::value pango(picojson::object_type,false);
    pango.get<picojson::object>()["version"] = picojson::value(PANGOLIN_VERSION_STRING);
    pango.get<picojson::object>()["date_created"] = picojson::value( CurrentTimeStr() );
    pango.get<picojson::object>()["endian"] = picojson::value("little_endian");

    WriteTag(TAG_PANGO_HDR);
    pango.serialize(std::ostream_iterator<char>(writer), true);
}

void PacketStreamWriter::WriteStats()
{
    WriteTag(TAG_PANGO_STATS);
    picojson::value stat(picojson::object_type,false);
    stat.get<picojson::object>()["num_sources"] = picojson::value((int64_t)sources.size());
    stat.get<picojson::object>()["bytes_written"] = picojson::value((int64_t)bytes_written);
    stat.serialize(std::ostream_iterator<char>(writer), true);
}

void PacketStreamWriter::WriteSync()
{
    for(int i=0; i<10; ++i) {
        WriteTag(TAG_PANGO_SYNC);
    }
}

//////////////////////////////////////////////////////////////////////////
// PacketStreamReader
//////////////////////////////////////////////////////////////////////////

PacketStreamReader::PacketStreamReader(const std::string& filename)
{
    next_tag[3] = '\n';

    const int magic_pango_len = 5;
    const unsigned char magic_pango[] = "PANGO";
    char buffer[magic_pango_len];

    reader.open(filename, std::ios::in | std::ios::binary);

    // Check file magic matches expected value
    reader.read(buffer,magic_pango_len);
    if(!strncmp((char*)buffer, (char*)magic_pango, magic_pango_len) ) {
        // Read opening file JSON.
        ReadHeaderPacket();

        ReadTag();

        // Read any source headers, stop on first frame
        while(!strncmp((char*)next_tag, (char*)TAG_ADD_SOURCE.c_str(), TAG_LENGTH))
        {
            ReadNewSourcePacket();
            ReadTag();
        }
    }else{
        throw std::runtime_error("Unrecognised or corrupted file header.");
    }
}

PacketStreamReader::~PacketStreamReader()
{
    reader.close();
}

void PacketStreamReader::RegisterSourceHeaderHandler(NewSourceReceiver &receiver )
{
    source_handlers.push_back(&receiver);

    // Inform of already loaded sources
    for(unsigned int s=0; s < sources.size(); ++s) {
        receiver.NewSource(s, sources[s]);
    }
}

void PacketStreamReader::RegisterFrameHandler( PacketStreamSourceId src_id )
{
    sources[src_id].registered_handler = true;
}

void PacketStreamReader::UnregisterFrameHandler( PacketStreamSourceId src_id )
{
    sources[src_id].registered_handler = false;
}

//void PacketStreamReader::RegisterFrameHandler( PacketStreamSourceId src_id, const FrameHandler& handler )
//{
//    if(src_id >= sources.size()) {
//        throw std::runtime_error("Invalid PacketStream source ID.");
//    }

//    frame_handlers[src_id] = handler;
//}

bool PacketStreamReader::ProcessUpToNextSourceFrame(PacketStreamSourceId src_id)
{
    int nxt = ProcessMessagesUntilRegisteredSourceFrame();
    if(nxt == -1) return false;

    while(nxt != (int)src_id) {
        ReadOverSourceFramePacket(src_id);
        nxt = ProcessMessagesUntilRegisteredSourceFrame();
        if(nxt == -1) return false;
    }
    return true;
}

// return src_id
int PacketStreamReader::ProcessMessagesUntilRegisteredSourceFrame()
{
    while(true)
    {
        // Read one frame / header
        if(!strncmp((char*)next_tag, (char*)TAG_PANGO_HDR.c_str(), TAG_LENGTH)) {
            ReadHeaderPacket();
        }else if(!strncmp((char*)next_tag, (char*)TAG_ADD_SOURCE.c_str(), TAG_LENGTH)) {
            ReadNewSourcePacket();
        }else if(!strncmp((char*)next_tag, (char*)TAG_PANGO_STATS.c_str(), TAG_LENGTH)) {
            ReadStatsPacket();
        }else if(!strncmp((char*)next_tag, (char*)TAG_SRC_FRAME.c_str(), TAG_LENGTH)) {
            const size_t src_id = ReadCompressedUnsignedInt();
            if(src_id >= sources.size()) {
                throw std::runtime_error("Invalid Frame Source ID.");
            }
            if(sources[src_id].registered_handler) {
                return src_id;
            }else{
                // We have to read over this frame.
                ReadOverSourceFramePacket(src_id);
            }
        }else if(!strncmp((char*)next_tag, (char*)TAG_END.c_str(), TAG_LENGTH)) {
            return -1;
        }else{
            // TODO: Resync
            throw std::runtime_error("Unknown packet type.");
        }
        if(!ReadTag()) {
            // Dummy end tag
            strcpy((char*)next_tag,TAG_END.c_str());
        }
    }
}

bool PacketStreamReader::ReadTag()
{
    if(reader.good()) {
        reader.read((char*)next_tag, TAG_LENGTH);
    }

    return reader.good();
}

void PacketStreamReader::ReadHeaderPacket()
{
    picojson::value json_header;
    picojson::parse(json_header, reader);

    // Consume newline
    char buffer[1];
    reader.read(buffer,1);

    std::cout << json_header.serialize(true) << std::endl;
}

void PacketStreamReader::ReadNewSourcePacket()
{
    const int src_id = sources.size();

    picojson::value json;
    picojson::parse(json, reader);
    reader.get(); // consume newline

    if(!json.contains(json_hdr_type) || !json.contains(json_hdr_uri)) {
        pango_print_error("Missing required fields for source.\n" );
    }

    picojson::value jstype = json.get(json_hdr_type);
    picojson::value jsuri  = json.get(json_hdr_uri);

    if( !jstype.is<std::string>() || !jsuri.is<std::string>() ) {
        pango_print_error("Missing required fields for source.\n" );
    }

    PacketStreamSource ps;
    ps.type = jstype.get<std::string>();
    ps.uri  = jsuri.get<std::string>();

    const std::string ns = ps.type+"::";

    if(json.contains(json_hdr_header)) {
        ps.header = json.get(json_hdr_header);
    }

    if(json.contains(json_hdr_typed_aux)) {
        picojson::value jsaux = json.get(json_hdr_typed_aux);
        if(jsaux.is<picojson::object>()) {
            typemap.AddTypes( ns, jsaux.get<picojson::object>() );
        }
    }

    if(json.contains(json_hdr_typed_frame)) {
        ps.frametype = typemap.CreateOrGetType(ns, json.get(json_hdr_typed_frame));
    }

    ps.registered_handler = false;
    sources.push_back(ps);

    std::cout << json.serialize(true) << std::endl;

    for(unsigned int r=0; r<source_handlers.size(); ++r) {
        source_handlers[r]->NewSource(src_id, sources[src_id]);
    }
}

void PacketStreamReader::ReadStatsPacket()
{
    picojson::value json;
    picojson::parse(json, reader);
    reader.get(); // consume newline
    std::cout << json.serialize(true) << std::endl;
}

void PacketStreamReader::ReadOverSourceFramePacket(PacketStreamSourceId src_id)
{
    const PacketStreamSource& src = sources[src_id];
    const PacketStreamType& src_type = typemap.GetType(src.frametype);
    if(src_type.IsFixedSize()) {
        reader.ignore(src_type.size_bytes);
    }else{
        size_t size_bytes = ReadCompressedUnsignedInt();
        reader.ignore(size_bytes);
    }
}


}