#include <string>

#include <Lintel/LintelLog.hpp>
#include <Lintel/PointerUtil.hpp>
#include <Lintel/RotatingHashMap.hpp>

#include <DataSeries/SequenceModule.hpp>
#include <analysis/nfs/common.hpp>

using namespace std;
using boost::format;
using dataseries::TFixedField;

// not intended for writing, leaves out packing options
const string attropscommonjoin_xml_in( 
    "<ExtentType name=\"attr-ops-join\">\n"
    "  <field type=\"int64\" name=\"request-at\" %1% />\n"
    "  <field type=\"int64\" name=\"reply-at\" %1% />\n"
    "  <field type=\"int32\" name=\"server\" />\n"
    "  <field type=\"int32\" name=\"client\" />\n"
    "  <field type=\"byte\" name=\"unified-op-id\" />\n"
    "  <field type=\"int64\" name=\"reply-id\" note=\"sorted by this\" />\n"
    "  <field type=\"variable32\" name=\"filename\" opt_nullable=\"yes\" />\n"
    "  <field type=\"variable32\" name=\"filehandle\" print_maybehex=\"yes\" />\n"
    "  <field type=\"variable32\" name=\"type\" />\n"
    "  <field type=\"int64\" name=\"file-size\" />\n"
    "  <field type=\"int64\" name=\"modify-time\" units=\"nanoseconds\" epoch=\"unix\" />\n"
    "  <field type=\"int32\" name=\"payload-length\" units=\"bytes\" />\n"
    "</ExtentType>\n");

// we cheat here and use the reply packet rather than the request
// packet because the request packets aren't quite sorted and we don't
// have a generic resort implemented, nor do I feel like writing a
// sort module here.

// TODO: redo-with rotating hash map.

// Note this join and the next one are tied together by a rw_side
// variable that is used because we really ought to be doing some sort
// of outer join on the attributes because we can end up having
// entries that only show up in the rw table rather than the common
// table, but we want to extract file size and modify time from it if
// possible.  This is kinda hacky.
class AttrOpsCommonJoin : public NFSDSModule {
  public:
    AttrOpsCommonJoin()
            : nfs_common(NULL), nfs_attrops(NULL),
              es_common(ExtentSeries::typeExact),
              es_attrops(ExtentSeries::typeExact),
              in_packetat(es_common,""),
              out_requestat(es_out,"request-at"),
              out_replyat(es_out,"reply-at"),
              in_source(es_common,"source"),
              out_server(es_out,"server"),
              in_dest(es_common,"dest"),
              out_client(es_out,"client"),
              in_is_request(es_common,""),
              in_op_id(es_common, "", Field::flag_nullable),
              in_nfs_version(es_common, "", Field::flag_nullable),
              out_unified_op_id(es_out,"unified-op-id"),
              in_recordid(es_common,""),
              in_requestid(es_attrops,""),
              in_replyid(es_attrops,""),
              out_replyid(es_out,"reply-id"),
              in_filename(es_attrops,"filename",Field::flag_nullable),
              out_filename(es_out,"filename",Field::flag_nullable),
              in_filehandle(es_attrops,"filehandle"),
              out_filehandle(es_out,"filehandle"),
              in_type(es_attrops,"type"),
              out_type(es_out,"type"),
              in_filesize(es_attrops,""),
              out_filesize(es_out,"file-size"),
              in_modifytime(es_attrops,""),
              out_modifytime(es_out,"modify-time"),
              in_payloadlen(es_common,""),
              out_payloadlen(es_out,"payload-length"),
              all_done(false), prev_replyid(-1),
              curreqht(new reqHashTable()),
              prevreqht(new reqHashTable()),
              prune_entries_after_use(false),
              last_rotate_time_raw(0),
              rotate_interval_seconds(5*60),
              rotate_interval_raw(0), 
              output_record_count(0), 
              force_1us_turnaround_count(0),
              output_bytes(0),
              in_initial_skip_mode(true),
              first_keep_time_raw(0),
              skipped_common_count(0), skipped_attrops_count(0), 
              skipped_duplicate_attr_count(0), 
              last_record_id(numeric_limits<int64_t>::min()),
              last_reply_id(numeric_limits<int64_t>::min()),
              enable_side_data(false),
              unified_read_id(nameToUnifiedId("read")),
              unified_write_id(nameToUnifiedId("write")),
              last_side_data_rotate(numeric_limits<int64_t>::min()),
              last_reported_memory_usage(0),
              readdirplus_unified_id(nameToUnifiedId("readdirplus")),
              readdirplus_missing_request_count(0)
    { 
        if (LintelLog::wouldDebug("memory_usage")) {
            last_reported_memory_usage = 1;
        } 
    }

    virtual ~AttrOpsCommonJoin() { 
        delete prevreqht;
        prevreqht = NULL;
        delete curreqht;
        curreqht = NULL;
    }

    void setInputs(DataSeriesModule &common, DataSeriesModule &attr_ops) {
        nfs_common = &common;
        nfs_attrops = &attr_ops;
    }

    void prepFields(Extent::Ptr e) {
        rw_side_data_thread = pthread_self();
        const ExtentType::Ptr type = e->getTypePtr();
        if (type->getName() == "NFS trace: common") {
            SINVARIANT(type->getNamespace() == "" &&
                       type->majorVersion() == 0 &&
                       type->minorVersion() == 0);
            // This version of the converter could generate duplicate
            // entries in the attr-ops table because it matched
            // replies to the last request.  However, the conversion
            // was over short files, so we don't have to keep things
            // for very long (although it would be better to rotate on
            // counts rather than time).
            prune_entries_after_use = false;
            rotate_interval_seconds = 5*60;
            in_packetat.setFieldName("packet-at");
            in_is_request.setFieldName("is-request");
            in_op_id.setFieldName("op-id");
            in_nfs_version.setFieldName("nfs-version");
            in_recordid.setFieldName("record-id");
            in_requestid.setFieldName("request-id");
            in_replyid.setFieldName("reply-id");
            in_filesize.setFieldName("file-size");
            in_modifytime.setFieldName("modify-time");
            in_payloadlen.setFieldName("payload-length");
        } else if (type->getName() == "Trace::NFS::common"
                   && type->versionCompatible(1,0)) {
            // Later versions of the converter matched each reply to
            // exactly one request, so we can quickly prune entries,
            // however we can get longer delays because we processed
            // over more files.
            prune_entries_after_use = true;
            rotate_interval_seconds = 20*60;
            in_packetat.setFieldName("packet-at");
            in_is_request.setFieldName("is-request");
            in_op_id.setFieldName("op-id");
            in_nfs_version.setFieldName("nfs-version");
            in_recordid.setFieldName("record-id");
            in_requestid.setFieldName("request-id");
            in_replyid.setFieldName("reply-id");
            in_filesize.setFieldName("file-size");
            in_modifytime.setFieldName("modify-time");
            in_payloadlen.setFieldName("payload-length");
        } else if (type->getName() == "Trace::NFS::common"
                   && type->versionCompatible(2,0)) {
            // Later versions of the converter matched each reply to
            // exactly one request, so we can quickly prune entries,
            // however we can get longer delays because we processed
            // over more files. (Saw a delay of ~9 minutes between the
            // request and the reply in nfs-2/set-3/117500-117999.ds
            // for req=87245463576,rep=87245497839)
            prune_entries_after_use = true;
            rotate_interval_seconds = 20*60;
            in_packetat.setFieldName("packet_at");
            in_is_request.setFieldName("is_request");
            in_op_id.setFieldName("op_id");
            in_nfs_version.setFieldName("nfs_version");
            in_recordid.setFieldName("record_id");
            in_requestid.setFieldName("request_id");
            in_replyid.setFieldName("reply_id");
            in_filesize.setFieldName("file_size");
            in_modifytime.setFieldName("modify_time");
            in_payloadlen.setFieldName("payload_length");
        } else {
            FATAL_ERROR("?");
        }
    }

    struct reqData {
        ExtentType::int64 request_id;
        Int64TimeField::Raw request_at_raw;
        uint8_t unified_op_id;
    };
    
    struct reqHash {
        unsigned operator()(const reqData &k) const {
            unsigned ret,a,b;
            a = (unsigned)(k.request_id >> 32);
            b = (unsigned)(k.request_id & 0xFFFFFFFF);
            ret = 1972;
            lintel_BobJenkinsHashMix(a,b,ret);
            return ret;
        }
    };

    struct reqEqual {
        bool operator()(const reqData &a, const reqData &b) const {
            return a.request_id == b.request_id;
        }
    };

    typedef HashTable<reqData,reqHash,reqEqual> reqHashTable;

    void initOutType() {
        SINVARIANT(es_common.getTypePtr() != NULL && es_attrops.getTypePtr() != NULL);
        
        string units_epoch;
        if (es_common.getTypePtr()->majorVersion() == 0) {
            SINVARIANT(es_attrops.getTypePtr()->majorVersion() == 0);
            units_epoch = "units=\"nanoseconds\" epoch=\"unix\"";
        } else if (es_common.getTypePtr()->majorVersion() == 1) { 
            SINVARIANT(es_attrops.getTypePtr()->majorVersion() == 1);
            units_epoch = "units=\"2^-32 seconds\" epoch=\"unix\"";
        } else if (es_common.getTypePtr()->majorVersion() == 2) {
            SINVARIANT(es_attrops.getTypePtr()->majorVersion() == 2);
            units_epoch = "units=\"2^-32 seconds\" epoch=\"unix\"";
        } else {
            FATAL_ERROR("don't know units and epoch");
        }
        string tmp = (boost::format(attropscommonjoin_xml_in) 
                      % units_epoch).str();
        es_out.setType(ExtentTypeLibrary::sharedExtentTypePtr(tmp));
    }

    void reportMemoryUsage() {
        size_t a = curreqht->memoryUsage();
        size_t b = prevreqht->memoryUsage();
        size_t c = rw_side_data.memoryUsage();
        LintelLogDebug("memory_usage", 
                       format("# Memory-Usage: AttrOpsCommonJoin %d = %d + %d + %d")
                       % (a + b + c) % a % b % c);
        last_reported_memory_usage = a + b + c;
    }
        
    void finalCommonSideData() {
        LintelLogDebug("AttrOpsCommonJoin", "finalCommonSideData");
        if (!es_common.morerecords()) {
            es_common.setExtent(nfs_common->getSharedExtent());
        }
        while (es_common.hasExtent()) {
            if (es_common.morerecords()) {
                
                uint8_t unified_id = opIdToUnifiedId(in_nfs_version.val(),
                                                     in_op_id.val());
                if (unified_id == unified_read_id || unified_id == unified_write_id) {
                    rw_side_data[in_recordid.val()] 
                            = RWSideData(in_packetat.valRaw(), in_source.val(),
                                         in_dest.val(), unified_id == unified_read_id);
                }
                ++es_common;
            } else {
                es_common.setExtent(nfs_common->getSharedExtent());
            }
        }
    }

    virtual Extent::Ptr getSharedExtent() {
        if (all_done) {
            reportMemoryUsage();
            LintelLogDebug("AttrOpsCommonJoin", "getExtent all-done-1");
            return Extent::Ptr();
        }
        if (last_reported_memory_usage > 0) {
            size_t memory_usage = curreqht->memoryUsage()
                                  + prevreqht->memoryUsage() + rw_side_data.memoryUsage();
            if (memory_usage > (last_reported_memory_usage + 4*1024*1024)) {
                reportMemoryUsage();
            }
        }
        if (!es_common.hasExtent()) {
            Extent::Ptr tmp = nfs_common->getSharedExtent();
            if (tmp == NULL) {
                all_done = true;
                es_attrops.clearExtent();
                reportMemoryUsage();
                LintelLogDebug("AttrOpsCommonJoin", "getExtent all-done-2");
                return Extent::Ptr();
            }

            if (in_packetat.getName().empty()) {
                prepFields(tmp);
            }

            es_common.setExtent(tmp);
            if (rotate_interval_raw == 0) {
                rotate_interval_raw = in_packetat.secNanoToRaw(rotate_interval_seconds,0);
            }
        }

        Extent::Ptr attrextent = nfs_attrops->getSharedExtent();
        if (attrextent == NULL) {
            if (enable_side_data) {
                finalCommonSideData();
                SINVARIANT(!es_common.hasExtent());
            }
            es_common.clearExtent();
            all_done = true;
            reportMemoryUsage();
            LintelLogDebug("AttrOpsCommonJoin", "getExtent all-done-3");
            return Extent::Ptr();
        }
        
        es_attrops.setExtent(attrextent);

        if (es_out.getTypePtr() == NULL) {
            initOutType();
            SINVARIANT(es_out.getTypePtr() != NULL);
        }

        Extent::Ptr outextent(new Extent(es_out.getTypePtr()));
        es_out.setExtent(outextent);

        outextent->extent_source = str(format("(join around %s + %s)") 
                                       % es_common.getExtentRef().extent_source
                                       % es_attrops.getExtentRef().extent_source);

        string fh;
        while (es_attrops.morerecords()) {
      restart:
            if (es_common.morerecords() == false) {
                Extent::Ptr tmp = nfs_common->getSharedExtent();
                if (tmp == NULL) {
                    es_common.clearExtent();
                    es_attrops.clearExtent();
                    return outextent;
                }
                es_common.setExtent(tmp);
                if (enable_side_data) {
                    if (last_side_data_rotate < (in_packetat.valRaw() - rotate_interval_raw)) {
                        LintelLogDebug("AttrOpsCommonJoin", "rotate-side-data");
                        rw_side_data.rotate();
                        last_side_data_rotate = in_packetat.valRaw();
                    }
                    LintelLogDebug("AttrOpsCommonJoin", format("side-data mem %d")
                                   % rw_side_data.memoryUsage());
                }
            }
            if (in_replyid.val() < prev_replyid) {
                if (in_replyid.val() == 59777664992LL && prev_replyid == 70490468000LL) {
                    // nfs-2/set-2/214000
                    prev_replyid = in_replyid.val();
                
                    INVARIANT(in_recordid.val() == 70490468001LL, format("? %d") % in_recordid.val());
                    ++es_common; // one record to skip
                    last_record_id = 59777664985LL;
                    SINVARIANT(!es_common.morerecords());
                    goto restart;
                }
                if (in_replyid.val() == 60191872278LL && prev_replyid == 70904675286LL) {
                    // nfs-2/set-2/217500
                    prev_replyid = in_replyid.val();
                    INVARIANT(in_recordid.val() == 60191872270LL, format("? %d") % in_recordid.val());
                    last_record_id = 60191872270LL;
                }
            }
            INVARIANT(in_replyid.val() >= prev_replyid, 
                      format("attr-ops not sorted by reply id? %lld < %lld") 
                      % in_replyid.val() % prev_replyid);
            prev_replyid = in_replyid.val();
            if (last_record_id > in_recordid.val()) {
                if ((in_recordid.val() == 37315642835LL && last_record_id == 37315642836LL) 
                    || (in_recordid.val() == 38615056294LL && last_record_id == 38615056295LL)) {
                    // first one nfs-1/set-17, beginning of 06349 vs. end of 06348; tolerate
                    // second one nfs-1/set-17, beginning of 09170 vs end of 09169
                } else {
                    FATAL_ERROR(format("%d > %d in %s") % last_record_id
                                % in_recordid.val() % es_common.getExtentRef().extent_source); 
                }
            }
            last_record_id = in_recordid.val();
            if (in_recordid.val() < in_replyid.val()) {
                SINVARIANT(!in_nfs_version.isNull() &&
                           !in_op_id.isNull());
                uint8_t unified_id = opIdToUnifiedId(in_nfs_version.val(),
                                                     in_op_id.val());
                if (in_is_request.val()) {
                    if (last_rotate_time_raw 
                        < (in_packetat.valRaw() - rotate_interval_raw)) {
                        // much cheaper than scanning through the hash
                        // table looking for old entries.
                        delete prevreqht;
                        prevreqht = curreqht;
                        curreqht = new reqHashTable();
                        last_rotate_time_raw = in_packetat.valRaw();
                    }
                    reqData d;
                    d.request_id = in_recordid.val();
                    d.request_at_raw = in_packetat.valRaw();
                    d.unified_op_id = unified_id;
                    curreqht->add(d);
                } else {
                    // reply common record entry that occurs before
                    // the first attr-ops entry we have; usually a
                    // result of common operations that have no
                    // attr-ops.
                    ++skipped_common_count;
                }
                if (enable_side_data && 
                    (unified_id == unified_read_id || unified_id == unified_write_id)) {
                    rw_side_data[in_recordid.val()] 
                            = RWSideData(in_packetat.valRaw(), in_source.val(),
                                         in_dest.val(), unified_id == unified_read_id);
                }
                ++es_common;
            } else if (in_initial_skip_mode && in_replyid.val() < in_recordid.val()) {
                // because we now prune some of the initial entries in
                // the common record extents so as to match exactly
                // the times that we want to capture, we now have a
                // chance that some of the attr-op reply ids that we
                // see at the beginning of the attr-op extent will not
                // match any of the entries.  However, they should all
                // come in a row right at the beginning, so check that
                // condition and allow the recordid == replyid check
                // fail if somehow this rule is violated.  This may
                // not actually be safe as there were some cases of
                // fairly substantial out of orderness in the data.
                ++es_attrops;
                ++skipped_attrops_count;
            } else if (in_replyid.val() == last_reply_id) {
                // This happens from the conversion of readdirs into
                // lots of attrops entries; not clear exactly what we
                // should do here.
                es_attrops.next();
                ++skipped_duplicate_attr_count;
            } else {
                if (in_initial_skip_mode) {
                    in_initial_skip_mode = false;
                    first_keep_time_raw = in_packetat.valRaw();
                }
                // Following can happen now that we generate multiple
                // attr-ops bits as a result of parsing all of the
                // readdir bits.

                INVARIANT(in_recordid.val() == in_replyid.val(),
                          format("mismatch on common(%d) and attr-ops(%d) tables")
                          % in_recordid.val() % in_replyid.val());
                INVARIANT(!in_is_request.val(), 
                          "request not response being joined");
                last_reply_id = in_replyid.val();

                reqData k;
                k.request_id = in_requestid.val();
                reqData *d = curreqht->lookup(k);
                if (d == NULL) {
                    //                  printf("ZZ plook %lld\n",k.request_id);
                    d = prevreqht->lookup(k);
                }
                if (d == NULL) {
                    ++skipped_common_count;
                    ++skipped_attrops_count;
                    // because of the initial common pruning, we can
                    // now get the case where the reply was in the
                    // acceptable set, but the request wasn't.
                    if (opIdToUnifiedId(in_nfs_version.val(), in_op_id.val()) 
                        == readdirplus_unified_id) {
                        // For reasons I don't fully understand,
                        // readdirplus entries can have duplicates in
                        // the tables.  I believe this was a
                        // conversion bug.  See revision
                        // 5af64aa9862eb4a9918c9c87704bb11bf794dafd
                        // of nettrace2ds.cpp for discussion.
                        ++readdirplus_missing_request_count;
                    } else {
                        INVARIANT(// assume request took at most 30 seconds to process; rare so we don't try to be efficient
                            (in_packetat.valRaw() - first_keep_time_raw) 
                            < in_packetat.secNanoToRaw(30,0),
                            format("bad missing request %d for reply %d, too old %d - %d = %d")
                            % in_requestid.val() % in_replyid.val() 
                            % in_packetat.valRaw() % first_keep_time_raw  
                            % (in_packetat.valRaw() - first_keep_time_raw));
                    }
                    ++es_common;
                    ++es_attrops;
                    continue;
                }
                es_out.newRecord();
                ++output_record_count;
                out_requestat.setRaw(d->request_at_raw);
                out_replyat.setRaw(in_packetat.valRaw());
                out_unified_op_id.set(d->unified_op_id);
                if (in_packetat.valRaw() <= d->request_at_raw) {
                    if (false) {
                        cerr << format("Warning: %d <= %d on ids %d/%d; forcing 1us turnaround\n")
                                % d->request_at_raw % in_packetat.valRaw() 
                                % in_requestid.val() % in_replyid.val();
                    }
                    out_replyat.setRaw(d->request_at_raw + in_packetat.secNanoToRaw(0,1000));
                    ++force_1us_turnaround_count;
                }
                out_server.set(in_source.val());
                out_client.set(in_dest.val());
                out_replyid.set(in_replyid.val());
                if (in_filename.isNull()) {
                    out_filename.setNull(true);
                } else {
                    out_filename.set(in_filename);
                }
                out_filehandle.set(in_filehandle);
                out_type.set(in_type);
                out_filesize.set(in_filesize.val());
                out_modifytime.set(in_modifytime.val());
                out_payloadlen.set(in_payloadlen.val());

                if (enable_side_data && prune_entries_after_use
                    && (d->unified_op_id == unified_read_id || d->unified_op_id == unified_write_id)) {
                    rw_side_data.remove(in_requestid.val());
                }
                if (prune_entries_after_use) {
                    curreqht->remove(k, false);
                    prevreqht->remove(k, false);
                }
                ++es_common;
                ++es_attrops;
            }
        }

        es_attrops.clearExtent();
        output_bytes += outextent->size();
        return outextent;
    }

    virtual void printResult() {
        cout << format("Begin-%s\n") % __PRETTY_FUNCTION__;
        if (output_record_count == 0) {
            cout << "  No output from Join.\n";
        } else {
            cout << format("  generated %.3f million records, %.2f MB of extent output\n")
                    % ((double)output_record_count/1000000.0)
                    % ((double)output_bytes/(1024.0*1024.0));
            cout << format("  %lld records, or %.4f%% forced to 1us turnaround from previous 0 or negative turnaround\n")
                    % force_1us_turnaround_count
                    % (100.0*(double)force_1us_turnaround_count/(double)output_record_count);
            cout << format("  %d skipped common, %d skipped attrops, %d semi-duplicate attrops, first keep %ss\n")
                    % skipped_common_count % skipped_attrops_count % skipped_duplicate_attr_count
                    % in_packetat.rawToStrSecNano(first_keep_time_raw);
        }
        if (readdirplus_missing_request_count > 0) {
            cout << format("  %d missing readdir plus requests (all tested cases were duplicates)\n")
                    % readdirplus_missing_request_count;
        }
        cout << format("End-%s\n") % __PRETTY_FUNCTION__;
    }

    struct RWSideData {
        int64_t at;
        int32_t source_ip, dest_ip;
        bool is_read;
        RWSideData() : at(0), source_ip(0), dest_ip(0), is_read(false) { }
        RWSideData(int64_t a, int32_t b, int32_t c, bool d)
                : at(a), source_ip(b), dest_ip(c), is_read(d) { }
    };

    const RWSideData *getRWSideData(int64_t record_id) {
        SINVARIANT(rw_side_data_thread == pthread_self());
        RWSideData *ret = rw_side_data.lookup(record_id);
        return ret;
    }

    void removeRWSideData(int64_t record_id) {
        rw_side_data.remove(record_id);
    }

    void enableSideData() {
        enable_side_data = true;
    }

  private:
    DataSeriesModule *nfs_common, *nfs_attrops;
    ExtentSeries es_common, es_attrops, es_out;
    Int64TimeField in_packetat, out_requestat, out_replyat;
    Int32Field in_source, out_server;
    Int32Field in_dest, out_client;
    BoolField in_is_request;
    ByteField in_op_id, in_nfs_version, out_unified_op_id;
    Int64Field in_recordid, in_requestid, in_replyid, out_replyid;

    Variable32Field in_filename, out_filename, in_filehandle, out_filehandle, in_type, out_type;
    Int64Field in_filesize, out_filesize, in_modifytime, out_modifytime;
    Int32Field in_payloadlen, out_payloadlen;
    bool all_done;
    ExtentType::int64 prev_replyid;
    vector<string> ignore_filehandles;
    reqHashTable *curreqht, *prevreqht;
    bool prune_entries_after_use;
    Int64TimeField::Raw last_rotate_time_raw;
    uint32_t rotate_interval_seconds;
    Int64TimeField::Raw rotate_interval_raw;

    ExtentType::int64 output_record_count, force_1us_turnaround_count, 
                output_bytes;
    bool in_initial_skip_mode;
    ExtentType::int64 first_keep_time_raw;
    uint64_t skipped_common_count, skipped_attrops_count, 
                skipped_duplicate_attr_count;
    int64_t last_record_id, last_reply_id;

    bool enable_side_data;
    RotatingHashMap<int64_t, RWSideData> rw_side_data;
    pthread_t rw_side_data_thread; // safety
    uint8_t unified_read_id, unified_write_id;
    int64_t last_side_data_rotate; // raw units

    size_t last_reported_memory_usage;
    uint8_t readdirplus_unified_id;
    uint32_t readdirplus_missing_request_count;
};
        
namespace NFSDSAnalysisMod {
    NFSDSModule *newAttrOpsCommonJoin() {
        return new AttrOpsCommonJoin();
    }

    void setAttrOpsSources(DataSeriesModule *join, 
                           SequenceModule &common_seq, 
                           SequenceModule &attrops_seq) {
        lintel::safeDownCast<AttrOpsCommonJoin>(join)
                ->setInputs(common_seq.tail(), attrops_seq.tail());
    }
}


// not intended for writing, leaves out packing options
const string commonattrrw_xml_in( 
    "<ExtentType name=\"common-attr-rw-join\">\n"
    "  <field type=\"int64\" %1% name=\"request_at\" />\n"
    "  <field type=\"int64\" %1% name=\"reply_at\" note=\"almost sorted by this\" />\n"
    "  <field type=\"int32\" name=\"server\" />\n"
    "  <field type=\"int32\" name=\"client\" />\n"
    "  <field type=\"variable32\" name=\"filehandle\" print_hex=\"yes\" />\n"
    "  <field type=\"bool\" name=\"is_read\" />\n"
    "  <field type=\"int64\" name=\"file_size\" opt_nullable=\"yes\" />\n"
    "  <field type=\"int64\" name=\"modify_time\" opt_nullable=\"yes\" />\n"
    "  <field type=\"int64\" name=\"offset\" />\n"
    "  <field type=\"int32\" name=\"bytes\" />\n"
    "</ExtentType>\n"
                                  );

// Old implementation in 87e66db0041a3b1ffa81b0865efc06772e325c23 and
// prior revisions.
class CommonAttrRWJoin: public NFSDSModule {
  public:
    CommonAttrRWJoin()
            : commonattr(NULL), rw(NULL),
              es_commonattr(ExtentSeries::typeExact),
              es_rw(ExtentSeries::typeExact),
              in_request_at(es_commonattr,"request-at"),
              out_request_at(es_out,"request_at"),
              in_reply_at(es_commonattr,"reply-at"),
              out_reply_at(es_out,"reply_at"),
              in_server(es_commonattr,"server"),
              out_server(es_out,"server"),
              in_client(es_commonattr,"client"),
              out_client(es_out,"client"),
              in_unified_op_id(es_commonattr,"unified-op-id"),
              in_ca_reply_id(es_commonattr, "reply-id"),
              in_rw_request_id(es_rw, ""),
              in_rw_reply_id(es_rw, ""),
              in_ca_filehandle(es_commonattr,"filehandle"),
              in_rw_filehandle(es_rw,"filehandle"),
              out_filehandle(es_out,"filehandle"),
              in_filesize(es_commonattr,"file-size"),
              out_filesize(es_out,"file_size", Field::flag_nullable),
              in_modifytime(es_commonattr,"modify-time"),
              out_modifytime(es_out,"modify_time", Field::flag_nullable),
              in_offset(es_rw,"offset"),
              out_offset(es_out,"offset"),
              in_bytes(es_rw,"bytes"),
              out_bytes(es_out,"bytes"),
              in_is_read(es_rw,""),
              out_is_read(es_out,"is_read"),
              rw_done(false), commonattr_done(false),
              skip_count(0), last_rw_reply_id(-1),
              read_unified_id(nameToUnifiedId("read")),
              write_unified_id(nameToUnifiedId("write")),
              output_bytes(0), missing_attr_ops_in_join(0),
              did_field_names_init(false),
              common_attr_join(NULL)
    { }

    virtual ~CommonAttrRWJoin() { }

    void initOutType() {
        SINVARIANT(es_commonattr.getTypePtr() != NULL && es_out.getTypePtr() == NULL);

        xmlNodePtr ftype 
                = es_commonattr.getTypePtr()->xmlNodeFieldDesc("request-at");
        string units = ExtentType::strGetXMLProp(ftype, "units");
        string epoch = ExtentType::strGetXMLProp(ftype, "epoch");
        
        string repl = str(format("units=\"%s\" epoch=\"%s\"") % units % epoch);
        string tmp = str(format(commonattrrw_xml_in) % repl);
        es_out.setType(ExtentTypeLibrary::sharedExtentTypePtr(tmp));
    }

    void setInputs(DataSeriesModule *ca_join,
                   DataSeriesModule &common_attr_mod, 
                   DataSeriesModule &rw_mod) {
        common_attr_join = lintel::safeDownCast<AttrOpsCommonJoin>(ca_join);
        common_attr_join->enableSideData();
        commonattr = &common_attr_mod;
        rw = &rw_mod;
    }

    void doFieldNameInit(Extent &e) {
        did_field_names_init = true;

        const ExtentType::Ptr type = e.getTypePtr();
        if (type->getName() == "NFS trace: read-write") {
            SINVARIANT(type->getNamespace() == "" &&
                       type->majorVersion() == 0 &&
                       type->minorVersion() == 0);
            in_rw_request_id.setFieldName("request-id");
            in_rw_reply_id.setFieldName("reply-id");
            in_is_read.setFieldName("is-read");
        } else if (type->getName() == "Trace::NFS::read-write"
                   && type->versionCompatible(1,0)) {
            in_rw_request_id.setFieldName("request-id");
            in_rw_reply_id.setFieldName("reply-id");
            in_is_read.setFieldName("is-read");
        } else if (type->getName() == "Trace::NFS::read-write"
                   && type->versionCompatible(2,0)) {
            in_rw_request_id.setFieldName("request_id");
            in_rw_reply_id.setFieldName("reply_id");
            in_is_read.setFieldName("is_read");
        } else {
            FATAL_ERROR("?");
        }

    }

    void nextRWExtent() {
        if (rw_done) {
            return;
        }
        Extent::Ptr tmp = rw->getSharedExtent();
        if (tmp == NULL) {
            rw_done = true;
        } else {
            LintelLogDebug("CommonAttrRWJoin", format("got rw extent %s:%d") % tmp->extent_source
                           % tmp->extent_source_offset);
            if (!did_field_names_init) {
                doFieldNameInit(*tmp);
            }
        }
        es_rw.setExtent(tmp);
    }

    void nextCommonAttrExtent() {
        if (commonattr_done) {
            return;
        }
        Extent::Ptr tmp = commonattr->getSharedExtent();
        if (tmp == NULL) {
            commonattr_done = true;
        } 
        es_commonattr.setExtent(tmp);
    }

    void finishCheck() {
        LintelLogDebug("CommonAttrRWJoin", format("finishCheck %d %d") 
                       % commonattr_done % rw_done);
        // should only have unmatched remaining RW entries.
        if (!rw_done && !es_rw.morerecords()) {
            nextRWExtent();
        }
        while (!rw_done) {
            SINVARIANT(es_rw.morerecords());
            LintelLogDebug("CommonAttrRWJoin", format("rw-unmatched-finish %d %d")
                           % in_rw_request_id.val() % in_rw_reply_id.val());
            handleRWUnmatched();
            ++es_rw;
            if (!es_rw.morerecords()) {
                nextRWExtent();
            }
        }


        if (!commonattr_done && !es_commonattr.morerecords()) {
            nextCommonAttrExtent();
        }
        // may have more attr ops, but no read/write ones.
        while (!commonattr_done) {
            SINVARIANT(es_commonattr.morerecords());
            SINVARIANT(in_unified_op_id.val() != read_unified_id &&
                       in_unified_op_id.val() != write_unified_id);
            ++es_commonattr;
            if (!es_commonattr.morerecords()) {
                nextCommonAttrExtent();
            }
        }
    }

    void handleRWUnmatched() {
        // This can happen because with NFSv3, read-write
        // operations are not required to have any attributes
        // in them.
        const AttrOpsCommonJoin::RWSideData *_request 
                = common_attr_join->getRWSideData(in_rw_request_id.val());
        const AttrOpsCommonJoin::RWSideData *_reply
                = common_attr_join->getRWSideData(in_rw_reply_id.val());
        if (_request == NULL || _reply == NULL) {
            SINVARIANT(es_rw.morerecords());
            if (in_rw_request_id.val() == 90869686608LL) {
                // hack, work around duplicate rw entry in nfs-2/set-3/140500-140999.ds
                // means we don't generate the second entry, but it's a duplicate anyway.
                ++es_rw;
                return;
            }
            if (in_rw_request_id.val() <= 19999849433LL && in_rw_reply_id.val() > 19999849433LL
                && es_rw.getExtentRef().extent_source.find("nfs-2.set-0.20k.ds") != string::npos) {
                // silently tolerate; nfssubset got this wrong for check-data/nfs-2.set-0.20k.ds
                ++es_rw;
                return;
            }
                
            FATAL_ERROR(format("Unable to find rw record entries in side data for reqid=%d (%p)/repid=%d (%p) -- %s, %d, %d, %d; around %s:%d; ca_reply=%d")
                        % in_rw_request_id.val() % static_cast<const void *>(_request)
                        % in_rw_reply_id.val() % static_cast<const void *>(_reply)
                        % hexstring(in_rw_filehandle.stringval()) % in_offset.val() 
                        % in_bytes.val() % in_is_read.val() % es_rw.getExtentRef().extent_source
                        % es_rw.getExtentRef().extent_source_offset 
                        % (es_commonattr.hasExtent() ? in_ca_reply_id.val() : 0));
        }
        const AttrOpsCommonJoin::RWSideData &request(*_request);
        const AttrOpsCommonJoin::RWSideData &reply(*_reply);

        ++missing_attr_ops_in_join;

        SINVARIANT(request.at < reply.at);
        SINVARIANT(request.source_ip == reply.dest_ip);
        SINVARIANT(request.dest_ip == reply.source_ip);
        SINVARIANT(request.is_read == reply.is_read);

        es_out.newRecord();
        out_request_at.set(request.at);
        out_reply_at.set(reply.at);
        out_server.set(request.dest_ip);
        out_client.set(request.source_ip);
        out_filehandle.set(in_rw_filehandle);
        out_filesize.setNull();
        out_modifytime.setNull();
        out_offset.set(in_offset.val());
        out_bytes.set(in_bytes.val());
        out_is_read.set(request.is_read);

        common_attr_join->removeRWSideData(in_rw_request_id.val());
        common_attr_join->removeRWSideData(in_rw_reply_id.val());
        ++es_rw;
    }

    virtual Extent::Ptr getSharedExtent() { 
        if (commonattr_done && rw_done) {
            return Extent::Ptr();
        }
        if (es_out.getTypePtr() == NULL) {
            SINVARIANT(!es_commonattr.hasExtent() && !es_rw.hasExtent());
            nextCommonAttrExtent();
            if (!es_commonattr.hasExtent()) {
                SINVARIANT(commonattr_done);
                return Extent::Ptr();
            }
            nextRWExtent();
            if (!es_rw.hasExtent()) {
                SINVARIANT(rw_done);
                return Extent::Ptr();
            }
            initOutType();
        }
        es_out.newExtent();
        es_out.getExtentRef().extent_source = str(format("(join around %s + %s)") 
                                                  % es_commonattr.getExtentRef().extent_source
                                                  % es_rw.getExtentRef().extent_source);

        while (es_out.getExtentRef().size() < 128*1024) {
      restart:
            if (!es_rw.hasExtent() || es_rw.morerecords() == false) {
                nextRWExtent();
            }
            if (!es_commonattr.hasExtent() || es_commonattr.morerecords() == false) {
                nextCommonAttrExtent();
            }
            if (rw_done || commonattr_done) {
                finishCheck();
                break;
            }

            while (in_ca_reply_id.val() < in_rw_reply_id.val()) {
                SINVARIANT(in_unified_op_id.val() != read_unified_id &&
                           in_unified_op_id.val() != write_unified_id);
                ++skip_count;
                ++es_commonattr;
                if (!es_commonattr.morerecords()) {
                    goto restart;
                }
            }
            if (last_rw_reply_id > in_rw_reply_id.val() 
                && ((last_rw_reply_id == 70490467989LL // nfs-2/set-2/214000
                     && in_rw_reply_id.val() == 59777665194LL)
                    || (last_rw_reply_id == 70904675283LL // nfs-2/set-2/217500
                        && in_rw_reply_id.val() == 60191872300LL))
                ) {
                cout << format("# tolerating backwardness %d > %d") % last_rw_reply_id % in_rw_reply_id.val();
                while (in_unified_op_id.val() != read_unified_id &&
                      in_unified_op_id.val() != write_unified_id) {
                    ++skip_count;
                    ++es_commonattr;
                    if (!es_commonattr.morerecords()) {
                        goto restart;
                    }
                }
            } else {
                INVARIANT(last_rw_reply_id < in_rw_reply_id.val(), 
                          format("read-write extent not sorted by reply-id? %d >= %d in %s")
                          % last_rw_reply_id % in_rw_reply_id.val()
                          % es_rw.getExtentRef().extent_source);
            }
            last_rw_reply_id = in_rw_reply_id.val();
            if (in_ca_reply_id.val() != in_rw_reply_id.val()) {
                handleRWUnmatched();
            } else {
                SINVARIANT(in_rw_filehandle.equal(in_ca_filehandle));
                es_out.newRecord();
                out_request_at.set(in_request_at.val());
                out_reply_at.set(in_reply_at.val());
                out_server.set(in_server.val());
                out_client.set(in_client.val());
                out_filehandle.set(in_ca_filehandle);
                out_filesize.set(in_filesize.val());
                out_modifytime.set(in_modifytime.val());
                out_offset.set(in_offset.val());
                out_bytes.set(in_bytes.val());
                out_is_read.set(in_is_read.val());
                ++es_commonattr;
                ++es_rw;
            }
        }

        output_bytes += es_out.getExtentRef().size();
        return es_out.getSharedExtent();
    }

    virtual void printResult() {
        cout << format("Begin-%s\n") % __PRETTY_FUNCTION__;
        cout << format("  skipped %d records in join -- not read or write\n") % skip_count;
        if (missing_attr_ops_in_join) {
            cout << format("  %d rw rows w/o attr-ops (not unusual for NFSv3)\n")
                    % missing_attr_ops_in_join;
            cout << "Enable LINTEL_LOG_DEBUG=CommonAttrRWJoin to debug\n";
        }
        cout << format("  generated %.2f MB of extent output\n")
                % (static_cast<double>(output_bytes)/(1024.0*1024.0));
        cout << format("End-%s\n") % __PRETTY_FUNCTION__;
    }
  private:
    DataSeriesModule *commonattr, *rw;
    ExtentSeries es_commonattr, es_rw, es_out;
    TFixedField<int64_t> in_request_at, out_request_at;
    TFixedField<int64_t> in_reply_at, out_reply_at;
    TFixedField<int32_t> in_server, out_server;
    TFixedField<int32_t> in_client, out_client;
    TFixedField<uint8_t> in_unified_op_id;
    TFixedField<int64_t> in_ca_reply_id, in_rw_request_id, in_rw_reply_id;
    Variable32Field in_ca_filehandle, in_rw_filehandle, out_filehandle; 
    TFixedField<int64_t> in_filesize;
    Int64Field out_filesize;
    TFixedField<int64_t> in_modifytime;
    Int64Field out_modifytime;
    TFixedField<int64_t> in_offset, out_offset;
    TFixedField<int32_t> in_bytes, out_bytes;
    BoolField in_is_read, out_is_read;

    bool rw_done, commonattr_done;
    int skip_count;
    int64_t last_rw_reply_id;

    uint8_t read_unified_id, write_unified_id;
    ExtentType::int64 output_bytes;
    uint32_t missing_attr_ops_in_join;
    bool did_field_names_init;
    AttrOpsCommonJoin *common_attr_join;
};

namespace NFSDSAnalysisMod {
    NFSDSModule *newCommonAttrRWJoin() {
        return new CommonAttrRWJoin();
    }

    void setCommonAttrRWSources(DataSeriesModule *join, 
                                SequenceModule &commonattr_seq,
                                SequenceModule &rw_seq) {
        lintel::safeDownCast<CommonAttrRWJoin>(join)
                ->setInputs(commonattr_seq.begin()->get(), commonattr_seq.tail(), rw_seq.tail());
    }
}

