#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <errno.h>
#include <pwd.h>
#include <unistd.h>

#include <new> // losertree.h needs this and forgot to include it.
#include <parallel/losertree.h>

#include <concurrency/ThreadManager.h>
#include <concurrency/PosixThreadFactory.h>
#include <protocol/TBinaryProtocol.h>
#include <server/TSimpleServer.h>
#include <transport/TServerSocket.h>
#include <transport/TTransportUtils.h>

#include <boost/foreach.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>

#include <Lintel/HashUnique.hpp>
#include <Lintel/LintelLog.hpp>
#include <Lintel/PriorityQueue.hpp>
#include <Lintel/ProgramOptions.hpp>
#include <Lintel/STLUtility.hpp>

#include <DataSeries/DSExpr.hpp>
#include <DataSeries/GeneralField.hpp>
#include <DataSeries/RowAnalysisModule.hpp>
#include <DataSeries/SequenceModule.hpp>
#include <DataSeries/TFixedField.hpp>
#include <DataSeries/TypeIndexModule.hpp>

#include "gen-cpp/DataSeriesServer.h"

using namespace std;
using namespace facebook::thrift;
using namespace facebook::thrift::protocol;
using namespace facebook::thrift::transport;
using namespace facebook::thrift::server;
using namespace dataseries;
using boost::shared_ptr;
using boost::format;
using boost::scoped_ptr;

class ThrowError {
public:
    void requestError(const string &msg) {
        LintelLog::warn(format("request error: %s") % msg);
        throw RequestError(msg);
    }

    void requestError(const format &fmt) {
        requestError(str(fmt));
    }

    void invalidTableName(const string &table, const string &msg) {
        LintelLog::warn(format("invalid table name '%s': %s") % table % msg);
        throw InvalidTableName(table, msg);
    }
};

// TODO: make this throw an error, and eventually figure out the right thing to have
// in AssertBoost.  Note that we want to be able to specify the class, have it automatically
// pick up file, line, expression, and any optional parameters (message, values, etc).
#define TINVARIANT(x) SINVARIANT(x)

class RenameCopier {
public:
    typedef shared_ptr<RenameCopier> Ptr;

    RenameCopier(ExtentSeries &source, ExtentSeries &dest)
        : source(source), dest(dest) { }

    void prep(const map<string, string> &copy_columns) {
        typedef map<string, string>::value_type vt;
        BOOST_FOREACH(const vt &copy_column, copy_columns) {
            source_fields.push_back(GeneralField::make(source, copy_column.first));
            dest_fields.push_back(GeneralField::make(dest, copy_column.second));
        }
    }

    void copyRecord() {
        SINVARIANT(!source_fields.empty() && source_fields.size() == dest_fields.size());
        for (size_t i = 0; i < source_fields.size(); ++i) {
            dest_fields[i]->set(source_fields[i]);
        }
    }

    ExtentSeries &source, &dest;
    vector<GeneralField::Ptr> source_fields;
    vector<GeneralField::Ptr> dest_fields;
};

class TeeModule : public RowAnalysisModule {
public:
    TeeModule(DataSeriesModule &source_module, const string &output_path)
	: RowAnalysisModule(source_module), output_path(output_path), 
	  output_series(), output(output_path, Extent::compress_lzf, 1),
	  output_module(NULL), copier(series, output_series), row_count(0), first_extent(false)
    { }

    virtual ~TeeModule() {
	delete output_module;
    }

    virtual void firstExtent(const Extent &e) {
	series.setType(e.getType());
	output_series.setType(e.getType());
        SINVARIANT(output_module == NULL);
	output_module = new OutputModule(output, output_series, e.getType(), 96*1024);
	copier.prep();
	ExtentTypeLibrary library;
	library.registerType(e.getType());
	output.writeExtentLibrary(library);
        first_extent = true;
    }

    virtual void processRow() {
	++row_count;
	output_module->newRecord();
	copier.copyRecord();
    }

    virtual void completeProcessing() {
        if (!first_extent) {
            SINVARIANT(row_count == 0);
            LintelLog::warn(format("no rows in %s") % output_path);
            ExtentTypeLibrary library;
            output.writeExtentLibrary(library);
        }
    }

    void close() {
        delete output_module;
        output_module = NULL;
        output.close();
    }

    const string output_path;
    ExtentSeries output_series;
    DataSeriesSink output;
    OutputModule *output_module;
    ExtentRecordCopy copier;
    uint64_t row_count;
    bool first_extent;
};

string renameField(const ExtentType *type, const string &old_name, const string &new_name) {
    string ret(str(format("  <field name=\"%s\"") % new_name));
    xmlNodePtr type_node = type->xmlNodeFieldDesc(old_name);
        
    for(xmlAttrPtr attr = type_node->properties; NULL != attr; attr = attr->next) {
        if (xmlStrcmp(attr->name, (const xmlChar *)"name") != 0) {
            // TODO: This can't be the right approach, but xmlGetProp() has an internal
            // function ~20 lines long to convert a property into a string. Maybe use
            // xmlAttrSerializeTxtContent somehow?
            xmlChar *tmp = xmlGetProp(type_node, attr->name);
            SINVARIANT(tmp != NULL);
            ret.append(str(format(" %s=\"%s\"") % attr->name % tmp));
            xmlFree(tmp);
        }
    }
    ret.append("/>\n");
    return ret;
}

class TableDataModule : public RowAnalysisModule {
public:
    TableDataModule(DataSeriesModule &source_module, TableData &into, uint32_t max_rows)
        : RowAnalysisModule(source_module), into(into), max_rows(max_rows)
    { 
        into.rows.reserve(max_rows < 4096 ? max_rows : 4096);
        into.more_rows = false;
    }

    virtual void firstExtent(const Extent &e) {
        series.setType(e.getType());
        const ExtentType &extent_type(e.getType());
        fields.reserve(extent_type.getNFields());
        for (uint32_t i = 0; i < extent_type.getNFields(); ++i) {
            string field_name(extent_type.getFieldName(i));
            fields.push_back(GeneralField::make(series, field_name));
            into.columns.push_back(TableColumn(field_name, 
                                               extent_type.getFieldTypeStr(field_name)));
        }
        into.__isset.columns = true;
    }

    virtual void processRow() {
        if (into.rows.size() == max_rows) {
            into.more_rows = true;
            into.__isset.more_rows = true;
            return;
        }
        into.rows.resize(into.rows.size() + 1);
        vector<string> &row(into.rows.back());
        row.reserve(fields.size());
        BOOST_FOREACH(GeneralField::Ptr g, fields) {
            row.push_back(g->val().valString());
        }
    }

    TableData &into;
    uint32_t max_rows;
    vector<GeneralField::Ptr> fields;
};

struct GVVec {
    vector<GeneralValue> vec;

    bool operator ==(const GVVec &rhs) const {
        return lintel::iteratorRangeEqual(vec.begin(), vec.end(), rhs.vec.begin(), rhs.vec.end());
    }

    uint32_t hash() const {
        uint32_t partial_hash = 1942;

        BOOST_FOREACH(const GeneralValue &gv, vec) {
            partial_hash = gv.hash(partial_hash);
        }
        return partial_hash;
    }

    void print (ostream &to) const {
        BOOST_FOREACH(const GeneralValue &gv, vec) {
            to << gv;
        }
    }

    void resize(size_t size) {
        vec.resize(size);
    }

    size_t size() const {
        return vec.size();
    }

    GeneralValue &operator [](size_t offset) {
        return vec[offset];
    }
    
    void extract(vector<GeneralField::Ptr> &fields) {
        SINVARIANT(fields.size() == vec.size());
        for (uint32_t i = 0; i < fields.size(); ++i) {
            vec[i].set(fields[i]);
        }
    }
};

inline ostream & operator << (ostream &to, GVVec &gvvec) {
    gvvec.print(to);
    return to;
}

class JoinModule : public DataSeriesModule, public ThrowError {
public:
    class Extractor {
    public:
        typedef boost::shared_ptr<Extractor> Ptr;

        Extractor(const string &into_field_name) : into_field_name(into_field_name), into() { }
        virtual ~Extractor() { }

        virtual void extract(const GVVec &a_val) = 0;

        const string into_field_name; 
        GeneralField::Ptr into;

        static void makeInto(vector<Ptr> &extractors, ExtentSeries &series) {
            BOOST_FOREACH(Ptr e, extractors) {
                SINVARIANT(e->into == NULL);
                e->into = GeneralField::make(series, e->into_field_name);
            }
        }
        
        static void extractAll(vector<Ptr> &extractors, const GVVec &lookup_val) {
            BOOST_FOREACH(Ptr e, extractors) {
                e->extract(lookup_val);
            }
        }
    };

    // Extract from a field and stuff it into a destination field
    class ExtractorField : public Extractor {
    public:
        virtual ~ExtractorField() { }
        static Ptr make(ExtentSeries &from_series, const string &from_field_name,
                        const string &into_field_name) {
            GeneralField::Ptr from(GeneralField::make(from_series, from_field_name));
            return Ptr(new ExtractorField(into_field_name, from));
        }

        // TODO: deprecate this version?
        static Ptr make(const string &field_name, GeneralField::Ptr from) {
            return Ptr(new ExtractorField(field_name, from));
        }
        virtual void extract(const GVVec &a_val) {
            into->set(from);
        }

    private:
        GeneralField::Ptr from;

        ExtractorField(const string &into_field_name, GeneralField::Ptr from) 
            : Extractor(into_field_name), from(from) 
        { }
    };

    // Extract from a value vector and stuff it into a destination field
    class ExtractorValue : public Extractor {
    public:
        virtual ~ExtractorValue() { }
        static Ptr make(const string &into_field_name, uint32_t pos) {
            return Ptr(new ExtractorValue(into_field_name, pos));
        }
        
        virtual void extract(const GVVec &a_val) {
            SINVARIANT(pos < a_val.size());
            into->set(a_val.vec[pos]);
        }

    private:
        uint32_t pos;

        ExtractorValue(const string &field_name, uint32_t pos) 
            : Extractor(field_name), pos(pos)
        { }
    };
};
        
// TODO: merge common code with StarJoinModule

class HashJoinModule : public JoinModule { 
public:
    typedef map<string, string> CMap; // column map

    HashJoinModule(DataSeriesModule &a_input, int32_t max_a_rows, DataSeriesModule &b_input,
                   const map<string, string> &eq_columns, const map<string, string> &keep_columns,
                   const string &output_table_name) 
        : a_input(a_input), b_input(b_input), max_a_rows(max_a_rows), 
          eq_columns(eq_columns), keep_columns(keep_columns), output_extent(NULL),
          output_table_name(output_table_name)
    { }

    static const string mapDGet(const map<string, string> &a_map, const string &a_key) {
        map<string, string>::const_iterator i = a_map.find(a_key);
        SINVARIANT(i != a_map.end());
        return i->second;
    }

    void firstExtent(const Extent &b_e) {
        ExtentSeries a_series;

        a_series.setExtent(a_input.getExtent());
        b_series.setType(b_e.getType());
        if (a_series.getExtent() == NULL) {
            requestError("a_table is empty?");
        }

        // Three possible sources for values in the output:
        // 
        // 1) the a value fields, so from the hash-map
        // 2a) the b fields, as one of the a eq fields.
        // 2b) the b fields, as one of the b values or eq fields
        // 
        // We do not try to optimize the extraction and just create a new general field for each
        // extraction so if the hash-map has duplicate columns, there will be duplicate fields.
        vector<GeneralField::Ptr> a_eq_fields;
        HashUnique<string> known_a_eq_fields;

        BOOST_FOREACH(const CMap::value_type &vt, eq_columns) {
            SINVARIANT(a_series.getType()->getFieldType(vt.first)
                       == b_series.getType()->getFieldType(vt.second));
            a_eq_fields.push_back(GeneralField::make(a_series, vt.first));
            b_eq_fields.push_back(GeneralField::make(b_series, vt.second));
            known_a_eq_fields.add(vt.first);
        }

        vector<GeneralField::Ptr> a_val_fields;
        HashMap<string, uint32_t> a_name_to_val_pos;

        BOOST_FOREACH(const CMap::value_type &vt, keep_columns) {
            if (prefixequal(vt.first, "a.")) {
                string field_name(vt.first.substr(2));
                if (!known_a_eq_fields.exists(field_name)) { // don't store eq fields we will
                                                             // access from the b eq fields
                    a_name_to_val_pos[field_name] = a_val_fields.size();
                    a_val_fields.push_back(GeneralField::make(a_series, field_name));
                }
            }
        }
        string output_xml(str(format("<ExtentType name=\"hash-join -> %s\""
                                     " namespace=\"server.example.com\" version=\"1.0\">\n")
                              % output_table_name));

        BOOST_FOREACH(const CMap::value_type &vt, keep_columns) {
            string field_name(vt.first.substr(2));
            string output_field_xml;

            if (prefixequal(vt.first, "a.") && a_name_to_val_pos.exists(field_name)) { // case 1
                TINVARIANT(a_series.getType()->hasColumn(field_name));
                output_field_xml = renameField(a_series.getType(), field_name, vt.second);
                extractors.push_back
                    (ExtractorValue::make(vt.second, a_name_to_val_pos[field_name]));
            } else if (prefixequal(vt.first, "a.") 
                       && eq_columns.find(field_name) != eq_columns.end()) { // case 2a
                const string b_field_name(eq_columns.find(field_name)->second);
                TINVARIANT(b_series.getType()->hasColumn(b_field_name));
                output_field_xml = renameField(a_series.getType(), field_name, vt.second);
                GeneralField::Ptr b_field(GeneralField::make(b_series, b_field_name));
                extractors.push_back(ExtractorField::make(vt.second, b_field));
            } else if (prefixequal(vt.first, "b.")
                       && b_series.getType()->hasColumn(field_name)) { // case 2b
                output_field_xml = renameField(b_series.getType(), field_name, vt.second);
                GeneralField::Ptr b_field(GeneralField::make(b_series, field_name));
                extractors.push_back(ExtractorField::make(vt.second, b_field));
            } else {
                requestError("invalid extraction");
            }
            output_xml.append(output_field_xml);
        }

        output_xml.append("</ExtentType>\n");
                    
        INVARIANT(!extractors.empty(), "must extract at least one field");
        int32_t row_count = 0;
        key.resize(a_eq_fields.size());
        GVVec val;
        val.resize(a_val_fields.size());
        while (1) {
            if (a_series.getExtent() == NULL) {
                break;
            }
            for(; a_series.more(); a_series.next()) {
                ++row_count;

                if (row_count >= max_a_rows) {
                    requestError("a table has too many rows");
                }
                key.extract(a_eq_fields);
                val.extract(a_val_fields);
                a_hashmap[key].push_back(val);
            }
            delete a_series.getExtent();
            a_series.setExtent(a_input.getExtent());
        }

        ExtentTypeLibrary lib;
        LintelLog::info(format("output xml: %s") % output_xml);
        output_series.setType(lib.registerTypeR(output_xml));
        output_extent = new Extent(*output_series.getType());
        
        Extractor::makeInto(extractors, output_series);
    }

    virtual Extent *getExtent() {
        while (true) {
            Extent *e = b_input.getExtent();
            if (e == NULL) {
                break;
            }
            if (output_series.getType() == NULL) {
                firstExtent(*e);
            }
        
            if (output_series.getExtent() == NULL) {
                SINVARIANT(output_series.getType() != NULL);
                Extent *output_extent = new Extent(*output_series.getType());
                output_series.setExtent(output_extent); 
            }
            for (b_series.setExtent(e); b_series.more(); b_series.next()) {
                processRow();
            }
            delete e;
            if (output_series.getExtent()->size() > 96*1024) {
                break;
            }
        }
        Extent *ret = output_series.getExtent();
        output_series.clearExtent();
        return ret;
    }

    void processRow() {
        key.extract(b_eq_fields);
        vector<GVVec> *v = a_hashmap.lookup(key);
        if (v != NULL) {
            cout << format("%d match on %s\n") % v->size() % key;
            BOOST_FOREACH(const GVVec &a_vals, *v) {
                output_series.newRecord();
                BOOST_FOREACH(Extractor::Ptr p, extractors) {
                    p->extract(a_vals);
                }
            }
        }
    }

    DataSeriesModule &a_input, &b_input;
    int32_t max_a_rows;
    const CMap eq_columns, keep_columns;
    
    HashMap< GVVec, vector<GVVec> > a_hashmap;
    GVVec key;
    ExtentSeries b_series, output_series;
    vector<GeneralField::Ptr> b_eq_fields;
    vector<Extractor::Ptr> extractors;
    Extent *output_extent;
    const string output_table_name;
    
};

class SelectModule : public DataSeriesModule {
public:
    SelectModule(DataSeriesModule &source, const string &where_expr_str)
        : source(source), where_expr_str(where_expr_str), copier(input_series, output_series)
    { }

    virtual ~SelectModule() { }

    Extent *returnOutputSeries() {
        Extent *ret = output_series.getExtent();
        output_series.clearExtent();
        return ret;
    }

    virtual Extent *getExtent() {
        while (true) {
            Extent *in = source.getExtent();
            if (in == NULL) {
                return returnOutputSeries();
            }
            if (input_series.getType() == NULL) {
                input_series.setType(in->getType());
                output_series.setType(in->getType());

                copier.prep();
                where_expr.reset(DSExpr::make(input_series, where_expr_str));
            }

            if (output_series.getExtent() == NULL) {
                output_series.setExtent(new Extent(*output_series.getType()));
            }
        
            for (input_series.setExtent(in); input_series.more(); input_series.next()) {
                if (where_expr->valBool()) {
                    output_series.newRecord();
                    copier.copyRecord();
                }
            }
            if (output_series.getExtent()->size() > 96*1024) {
                return returnOutputSeries();
            }
        }
    }

    DataSeriesModule &source;
    string where_expr_str;
    ExtentSeries input_series, output_series;
    ExtentRecordCopy copier;
    boost::shared_ptr<DSExpr> where_expr;
};

class ProjectModule : public DataSeriesModule {
public:
    ProjectModule(DataSeriesModule &source, const vector<string> &keep_columns)
        : source(source), keep_columns(keep_columns), copier(input_series, output_series)
    { }

    virtual ~ProjectModule() { }

    Extent *returnOutputSeries() {
        Extent *ret = output_series.getExtent();
        output_series.clearExtent();
        return ret;
    }

    void firstExtent(Extent &in) {
        const ExtentType &t(in.getType());
        input_series.setType(t);

        string output_xml(str(format("<ExtentType name=\"project (%s)\" namespace=\"%s\""
                                     " version=\"%d.%d\">\n") % t.getName() % t.getNamespace()
                              % t.majorVersion() % t.minorVersion()));
        HashUnique<string> kc;
        BOOST_FOREACH(const string &c, keep_columns) {
            kc.add(c);
        }
        for (uint32_t i = 0; i < t.getNFields(); ++i) {
            const string &field_name(t.getFieldName(i));
            if (kc.exists(field_name)) {
                output_xml.append(t.xmlFieldDesc(field_name));
            }
        }
        output_xml.append("</ExtentType>\n");
        ExtentTypeLibrary lib;
        const ExtentType &output_type(lib.registerTypeR(output_xml));
            
        output_series.setType(output_type);

        copier.prep();
    }

    virtual Extent *getExtent() {
        while (true) {
            Extent *in = source.getExtent();
            if (in == NULL) {
                return returnOutputSeries();
            }
            if (input_series.getType() == NULL) {
                firstExtent(*in);
            }

            if (output_series.getExtent() == NULL) {
                output_series.setExtent(new Extent(*output_series.getType()));
            }
        
            for (input_series.setExtent(in); input_series.more(); input_series.next()) {
                output_series.newRecord();
                copier.copyRecord();
            }
            if (output_series.getExtent()->size() > 96*1024) {
                return returnOutputSeries();
            }
        }
    }

    DataSeriesModule &source;
    ExtentSeries input_series, output_series;
    vector<string> keep_columns;
    ExtentRecordCopy copier;
};


class PrimaryKey {
public:
    PrimaryKey() { }

    void init(ExtentSeries &series, const vector<string> &field_names) {
        fields.reserve(field_names.size());
        BOOST_FOREACH(const string &name, field_names) {
            fields.push_back(GeneralField::make(series, name));
        }
    }

    /// Standard vector ordering by element
    bool operator <(const PrimaryKey &rhs) const {
        SINVARIANT(fields.size() == rhs.fields.size());
        for (size_t i=0; i < fields.size(); ++i) {
            if (*fields[i] < *rhs.fields[i]) {
                return true;
            } else if (*fields[i] > *rhs.fields[i]) {
                return false; // TODO: make sure we have a test for this case; it was missing without a "TEST" failing.
            } // else ==, keep looking
        }
        return false; // equal, so not less than
    }

    bool operator ==(const PrimaryKey &rhs) const {
        SINVARIANT(fields.size() == rhs.fields.size());
        for (size_t i=0; i < fields.size(); ++i) {
            if (*fields[i] != *rhs.fields[i]) {
                return false;
            }
        }
        return true;
    }

    void print(ostream &to) const {
        to << "(";
        BOOST_FOREACH(GeneralField::Ptr f, fields) {
            to << *f;
        }
        to << ")";
    }

    vector<GeneralField::Ptr> fields;
};

ostream &operator <<(ostream &to, const PrimaryKey &key) {
    key.print(to);
    return to;
}

class SortedUpdateModule : public DataSeriesModule, public ThrowError {
public:
    SortedUpdateModule(TypeIndexModule &base_input, TypeIndexModule &update_input,
                       const string &update_column, const vector<string> &primary_key) 
        : base_input(base_input), update_input(update_input), 
          base_series(), update_series(), output_series(),  base_copier(base_series, output_series),
          update_copier(update_series, output_series), update_column(update_series, update_column),
          primary_key_names(primary_key),  base_primary_key(), update_primary_key()
    { }

    inline bool outputExtentSmall() {
        return output_series.getExtent()->size() < 96 * 1024;
    }

    virtual Extent *getExtent() {
        processMergeExtents();
        if (output_series.getExtent() != NULL && outputExtentSmall()) {
            // something more to do..
            SINVARIANT(base_series.getExtent() == NULL || update_series.getExtent() == NULL);
            if (base_series.getExtent() != NULL) {
                processBaseExtents();
            } else if (update_series.getExtent() != NULL) {
                processUpdateExtents();
            } else {
                // both are done.
            }
        }

        return returnOutputSeries();
    }

    // TODO: lots of cut and paste, near identical code here; seems like we should be able
    // to be more efficient somehow.
    void processMergeExtents() {
        while (true) {
            if (base_series.getExtent() == NULL) {
                base_series.setExtent(base_input.getExtent());
            }
            if (update_series.getExtent() == NULL) {
                update_series.setExtent(update_input.getExtent());
            }

            if (base_series.getExtent() == NULL || update_series.getExtent() == NULL) {
                LintelLogDebug("SortedUpdate", "no more extents of one type");
                break;
            }

            if (output_series.getExtent() == NULL) {
                LintelLogDebug("SortedUpdate", "make output extent");
                output_series.setExtent(new Extent(base_series.getExtent()->getType()));
            }

            if (!base_series.more()) { // tolerate empty base_series extents
                delete base_series.getExtent();
                base_series.clearExtent();
                continue;
            }

            if (!update_series.more()) { // tolerate empty update_series extents
                delete update_series.getExtent();
                update_series.clearExtent();
                continue;
            }
            if (!outputExtentSmall()) {
                break;
            }

            if (base_primary_key.fields.empty()) {
                TINVARIANT(update_primary_key.fields.empty());
                base_primary_key.init(base_series, primary_key_names);
                update_primary_key.init(update_series, primary_key_names);
                base_copier.prep();
                update_copier.prep();
            }
           
            if (base_primary_key < update_primary_key) {
                copyBase();
                advance(base_series);
            } else {
                doUpdate();
                advance(update_series);
            }
        }
    }

    void processBaseExtents() {
        SINVARIANT(update_input.getExtent() == NULL);
        LintelLogDebug("SortedUpdate", "process base only...");
        while (true) {
            if (base_series.getExtent() == NULL) {
                base_series.setExtent(base_input.getExtent());
            }

            if (base_series.getExtent() == NULL) {
                LintelLogDebug("SortedUpdate", "no more base extents");
                break;
            }

            if (output_series.getExtent() == NULL) {
                LintelLogDebug("SortedUpdate", "make output extent");
                output_series.setExtent(new Extent(base_series.getExtent()->getType()));
            }

            if (!outputExtentSmall()) {
                break;
            }

            copyBase();
            advance(base_series);
        }            
    }

    void processUpdateExtents() {
        LintelLogDebug("SortedUpdate", "process update only...");
        while (true) {
            if (update_series.getExtent() == NULL) {
                update_series.setExtent(update_input.getExtent());
            }

            if (update_series.getExtent() == NULL) {
                LintelLogDebug("SortedUpdate", "no more update extents");
                break;
            }

            if (output_series.getExtent() == NULL) {
                LintelLogDebug("SortedUpdate", "make output extent");
                output_series.setExtent(new Extent(update_series.getExtent()->getType()));
            }

            if (!outputExtentSmall()) {
                break;
            }

            doUpdate();
            advance(update_series);
        }            
    }

    void copyBase() {
        LintelLogDebug("SortedUpdate", "copy-base");
        output_series.newRecord();
        base_copier.copyRecord();
    }

    void doUpdate() {
        switch (update_column()) 
            {
            case 1: doUpdateInsert(); break;
            case 2: doUpdateReplace(); break;
            case 3: doUpdateDelete(); break;
            default: requestError(format("invalid update column value %d")
                                  % static_cast<uint32_t>(update_column()));
            }
    }

    void doUpdateInsert() {
        copyUpdate();
    }

    void doUpdateReplace() {
        if (base_series.getExtent() != NULL && base_primary_key == update_primary_key) {
            copyUpdate();
            advance(base_series);
        } else {
            copyUpdate(); // equivalent to insert
        }
    }

    void doUpdateDelete() {
        if (base_series.getExtent() != NULL && base_primary_key == update_primary_key) {
            advance(base_series);
        } else {
            // already deleted
        }
    }

    void copyUpdate() {
        LintelLogDebug("SortedUpdate", "copy-update");
        output_series.newRecord();
        update_copier.copyRecord();
    }

    void advance(ExtentSeries &series) {
        series.next();
        if (!series.more()) {
            delete series.getExtent();
            series.clearExtent();
        }
    }

    Extent *returnOutputSeries() {
        Extent *ret = output_series.getExtent();
        output_series.clearExtent();
        return ret;
    }

    DataSeriesModule &base_input, &update_input;
    ExtentSeries base_series, update_series, output_series;
    ExtentRecordCopy base_copier, update_copier;
    TFixedField<uint8_t> update_column;
    const vector<string> primary_key_names;
    PrimaryKey base_primary_key, update_primary_key;
};

// In some ways this is very similar to the HashJoinModule, however one of the common data
// warehousing operations is to scan through a fact table adding in new columns based on one or
// more tiny dimension tables.  The efficient implementation is to load all of those tiny tables
// and make a single pass through the big table combining it with all of the tiny tables.
//
// To do that we will define a vector of Dimension(name, table, key = vector<column>, values =
// vector<column>) entries that define all of the dimension tables along with the key indexing into
// that dimension table, and a LookupIn(name, key = vector<column>, missing -> { skip_row |
// unchanged | value }, map<int, string> extract_values) that will lookup in JoinTo(name) using the
// two keys, and extract out a subset of the values from the JoinTo into the named columns.
// If the lookup is missing, it can either cause us to skip that row in the fact table, leave the
// output values unchanged, or set it to a specified value.  The middle option allows for lookups
// in multiple tables to select the last set value.
class StarJoinModule : public JoinModule {
public:
    StarJoinModule(DataSeriesModule &fact_input, const vector<Dimension> &dimensions,
                   const string &output_table_name, const map<string, string> &fact_columns,
                   const vector<DimensionFactJoin> &dimension_fact_join_in,
                   const HashMap< string, shared_ptr<DataSeriesModule> > &dimension_modules) 
        : fact_input(fact_input), dimensions(dimensions), output_table_name(output_table_name),
          fact_column_names(fact_columns), dimension_modules(dimension_modules)
    { 
        BOOST_FOREACH(const DimensionFactJoin &dfj, dimension_fact_join_in) {
            SJM_Join::Ptr p(new SJM_Join(dfj));
            dimension_fact_join.push_back(p);
        }
    }

    struct SJM_Dimension : public Dimension {
        typedef boost::shared_ptr<SJM_Dimension> Ptr;

        SJM_Dimension(Dimension &d) : Dimension(d), dimension_type() { }

        size_t valuePos(const string &source_field_name) {
            for(size_t i = 0; i < value_columns.size(); ++i) {
                if (value_columns[i] == source_field_name) {
                    return i;
                }
            }
            return numeric_limits<size_t>::max();
        }

        const ExtentType *dimension_type;
        HashMap<GVVec, GVVec> dimension_data; // map dimension key to dimension values
    };

    struct SJM_Join : public DimensionFactJoin {
        typedef boost::shared_ptr<SJM_Join> Ptr;
        
        SJM_Join(const DimensionFactJoin &d) : DimensionFactJoin(d), dimension(), join_data() { }
        virtual ~SJM_Join() throw () { }
        SJM_Dimension::Ptr dimension;
        vector<GeneralField::Ptr> fact_fields;
        vector<Extractor::Ptr> extractors;
        GVVec join_key; // avoid constant resizes, just overwrite
        GVVec *join_data; // temporary for the two-phase join
    };

    class DimensionModule : public RowAnalysisModule {
    public:
        typedef boost::shared_ptr<DimensionModule> Ptr;

        static Ptr make(DataSeriesModule &source, SJM_Dimension &sjm_dimension) {
            Ptr ret(new DimensionModule(source, sjm_dimension));
            return ret;
        }

        void firstExtent(const Extent &e) {
            LintelLogDebug("StarJoinModule/DimensionModule", format("first extent for %s via %s")
                           % dim.dimension_name % dim.source_table);
            key_gv.resize(dim.key_columns.size());
            value_gv.resize(dim.value_columns.size());
            series.setType(e.getType());
            BOOST_FOREACH(string key, dim.key_columns) {
                key_gf.push_back(GeneralField::make(series, key));
            }
            BOOST_FOREACH(string value, dim.value_columns) {
                value_gf.push_back(GeneralField::make(series, value));
            }
            SINVARIANT(dim.dimension_type == NULL);
            dim.dimension_type = series.getType();
        }

        void processRow() {
            key_gv.extract(key_gf);
            value_gv.extract(value_gf);
            dim.dimension_data[key_gv] = value_gv;
            LintelLogDebug("StarJoinModule/Dimension", format("[%d] -> [%d]") % key_gv % value_gv);
        }

    protected:
        DimensionModule(DataSeriesModule &source, SJM_Dimension &sjm_dimension) 
            : RowAnalysisModule(source), dim(sjm_dimension)
        { }

        SJM_Dimension &dim;

        vector<GeneralField::Ptr> key_gf, value_gf;
        GVVec key_gv, value_gv;
    };
            
    void processDimensions() {
        // TODO: probably could do this in only two loops rather than three, but this
        // is simpler for now, so leave it alone.
        name_to_sjm_dim.reserve(dimensions.size());
        BOOST_FOREACH(const SJM_Join::Ptr dfj, dimension_fact_join) {
            name_to_sjm_dim[dfj->dimension_name].reset(); // mark used dimensions
        }
        BOOST_FOREACH(Dimension &dim, dimensions) {
            if (name_to_sjm_dim.exists(dim.dimension_name)) {
                name_to_sjm_dim[dim.dimension_name].reset(new SJM_Dimension(dim));
            } else {
                requestError(format("unused dimension %s specified") % dim.dimension_name);
            }
        }

        BOOST_FOREACH(const SJM_Join::Ptr dfj, dimension_fact_join) {
            dfj->dimension = name_to_sjm_dim[dfj->dimension_name];
            if (dfj->dimension == NULL) {
                requestError(format("unspecified dimension %s used") % dfj->dimension_name);
            }
        }

        // TODO: check for identical source table + same keys, never makes sense, better to
        // just pull multiple values in that case.
        BOOST_FOREACH(string source_table, dimension_modules.keys()) {
            // Will be reading same extent multiple times for different dimensions. Little bit
            // inefficient but easy to implement.
            SequenceModule seq(dimension_modules[source_table]);
            BOOST_FOREACH(Dimension &dim, dimensions) {
                if (dim.source_table == source_table) {
                    LintelLogDebug("StarJoinModule", format("loading %s from %s") 
                                   % dim.dimension_name % dim.source_table);
                    seq.addModule(DimensionModule::make(seq.tail(), 
                                                        *name_to_sjm_dim[dim.dimension_name]));
                }
            }

            if (seq.size() == 1) {
                requestError(format("Missing dimensions using source table %s") % source_table);
            }
            seq.getAndDelete();
        }
        
        dimension_modules.clear();
    }

    void setOutputType() {
        SINVARIANT(fact_series.getType() != NULL);
        string output_xml(str(format("<ExtentType name=\"star-join -> %s\""
                                     " namespace=\"server.example.com\" version=\"1.0\">\n")
                              % output_table_name));
        typedef map<string, string>::value_type ss_pair;
        BOOST_FOREACH(ss_pair &v, fact_column_names) {
            TINVARIANT(fact_series.getType()->hasColumn(v.first));
            output_xml.append(renameField(fact_series.getType(), v.first, v.second));
            fact_fields.push_back(ExtractorField::make(fact_series, v.first, v.second));
        }
        BOOST_FOREACH(const SJM_Join::Ptr dfj, dimension_fact_join) {
            SJM_Dimension::Ptr dim = name_to_sjm_dim[dfj->dimension_name];
            SINVARIANT(dim->dimension_type != NULL);

            dfj->join_key.resize(dfj->fact_key_columns.size());
            BOOST_FOREACH(const string &fact_col_name, dfj->fact_key_columns) {
                dfj->fact_fields.push_back(GeneralField::make(fact_series, fact_col_name));
            }
            BOOST_FOREACH(ss_pair &ev, dfj->extract_values) {
                TINVARIANT(dim->dimension_type->hasColumn(ev.first));
                size_t value_pos = dim->valuePos(ev.first);
                TINVARIANT(value_pos != numeric_limits<size_t>::max());
                output_xml.append(renameField(dim->dimension_type, ev.first, ev.second));
                dfj->extractors.push_back(ExtractorValue::make(ev.second, value_pos));
            }
        }
        output_xml.append("</ExtentType>\n");
        LintelLogDebug("StarJoinModule", format("constructed output type:\n%s") % output_xml);

        ExtentTypeLibrary lib;
        output_series.setType(lib.registerTypeR(output_xml));

        Extractor::makeInto(fact_fields, output_series);
        BOOST_FOREACH(const SJM_Join::Ptr dfj, dimension_fact_join) {
            Extractor::makeInto(dfj->extractors, output_series);
        }
    }

    void firstExtent(const Extent &fact_extent) {
        processDimensions();
        fact_series.setType(fact_extent.getType());
        setOutputType();
    }

    virtual Extent *getExtent() {
        scoped_ptr<Extent> e(fact_input.getExtent());
        if (e == NULL) {
            return NULL;
        }
        if (output_series.getType() == NULL) {
            firstExtent(*e);
        }
        
        output_series.setExtent(new Extent(output_series));
        // TODO: do the resize to ~64-96k trick here rather than one in one out.
        for (fact_series.setExtent(e.get()); fact_series.more(); fact_series.next()) {
            processRow();
        }

        Extent *ret = output_series.getExtent();
        output_series.clearExtent();
        return ret;
    }

    void processRow() {
        // Two phases, in the first phase we look up everything and verify that we're going
        // to generate an output row.   In the second phase we generate the output row.

        BOOST_FOREACH(SJM_Join::Ptr join, dimension_fact_join) {
            join->join_key.extract(join->fact_fields);
            HashMap<GVVec, GVVec>::iterator i 
                = join->dimension->dimension_data.find(join->join_key);
            if (i == join->dimension->dimension_data.end()) {
                FATAL_ERROR(format("unimplemented; unable to find %s in dimension") % join->join_key);
            } else {
                SINVARIANT(join->join_data == NULL);
                join->join_data = &i->second;
            }
        }

        // All dimensions exist, make an output row.
        GVVec empty;
        output_series.newRecord();
        Extractor::extractAll(fact_fields, empty);
        BOOST_FOREACH(SJM_Join::Ptr join, dimension_fact_join) {
            SINVARIANT(join->join_data != NULL);
            Extractor::extractAll(join->extractors, *join->join_data);
            join->join_data = NULL;
        }
    }

    DataSeriesModule &fact_input;
    vector<Dimension> dimensions;
    string output_table_name;
    map<string, string> fact_column_names;
    ExtentSeries fact_series, output_series;
    vector<SJM_Join::Ptr> dimension_fact_join;
    HashMap< string, shared_ptr<DataSeriesModule> > dimension_modules; // source table to typeindexmodule
    HashMap<string, SJM_Dimension::Ptr> name_to_sjm_dim; // dimension name to SJM dimension
    vector<Extractor::Ptr> fact_fields;
};    

class UnionModule : public DataSeriesModule {
public:
    struct UM_UnionTable : UnionTable {
        UM_UnionTable() : UnionTable(), source(), series(), copier(), order_fields() { }
        UM_UnionTable(const UnionTable &ut, DataSeriesModule::Ptr source) 
            : UnionTable(ut), source(source), series(), copier(), order_fields() { }

        ~UM_UnionTable() throw () {}

        DataSeriesModule::Ptr source;
        ExtentSeries series;
        RenameCopier::Ptr copier;
        vector<GeneralField::Ptr> order_fields;
    };

    struct Compare {
        Compare(const vector<UM_UnionTable> &sources) : sources(sources) { }
        bool operator()(uint32_t ia, uint32_t ib) {
            const UM_UnionTable &a(sources[ia]);
            const UM_UnionTable &b(sources[ib]);
            SINVARIANT(a.series.getExtent() != NULL);
            SINVARIANT(b.series.getExtent() != NULL);
            SINVARIANT(a.order_fields.size() == b.order_fields.size());
            for (size_t i = 0; i < a.order_fields.size(); ++i)  {
                if (*a.order_fields[i] < *b.order_fields[i]) {
                    return false;
                } else if (*b.order_fields[i] < *a.order_fields[i]) {
                    return true;
                }
            }
            // They are equal, order by union position
            return ia >= ib;
        }
    private:
        const vector<UM_UnionTable> &sources;
    };

    UnionModule(const vector<UM_UnionTable> &in_sources, const vector<string> &order_columns,
                const string &output_table_name) 
        : output_series(), sources(in_sources), compare(sources), queue(compare, sources.size()),
          order_columns(order_columns), output_table_name(output_table_name)
    {
        SINVARIANT(!sources.empty());
    }

    virtual ~UnionModule() { }

    Extent *returnOutputSeries() {
        Extent *ret = output_series.getExtent();
        output_series.clearExtent();
        return ret;
    }

    void firstExtent() {
        map<string, string> output_name_to_type;
        typedef map<string, string>::value_type ss_vt;

        BOOST_FOREACH(UM_UnionTable &ut, sources) {
            SINVARIANT(ut.copier == NULL);
            ut.copier.reset(new RenameCopier(ut.series, output_series));

            SINVARIANT(ut.source != NULL);
            ut.series.setExtent(ut.source->getExtent());
            if (ut.series.getExtent() == NULL) {
                continue; // no point in making the other bits
            }
            
            map<string, string> out_to_in;
            BOOST_FOREACH(const ss_vt &v, ut.extract_values) {
                out_to_in[v.second] = v.first;
                string &output_type(output_name_to_type[v.second]);
                string renamed_output_type(renameField(ut.series.getType(), v.first, v.second));
                if (output_type.empty()) {
                    output_type = renamed_output_type;
                } else {
                    TINVARIANT(output_type == renamed_output_type);
                }
            }
            BOOST_FOREACH(const string &col, order_columns) {
                map<string, string>::iterator i = out_to_in.find(col);
                SINVARIANT(i != out_to_in.end());
                ut.order_fields.push_back(GeneralField::make(ut.series, i->second));
            }
            LintelLogDebug("UnionModule", format("made union stuff on %s") % ut.table_name);
        }

        string output_xml(str(format("<ExtentType name=\"union -> %s\""
                                     " namespace=\"server.example.com\" version=\"1.0\">\n")
                              % output_table_name));

        BOOST_FOREACH(const ss_vt &v, output_name_to_type) {
            output_xml.append(v.second);
        }
        output_xml.append("</ExtentType>\n");
        LintelLogDebug("UnionModule", format("constructed output type:\n%s") % output_xml);
        ExtentTypeLibrary lib;
        output_series.setType(lib.registerTypeR(output_xml));
        for (uint32_t i = 0; i < sources.size(); ++i) {
            UM_UnionTable &ut(sources[i]);
            if (ut.series.getExtent() != NULL) {
                ut.copier->prep(ut.extract_values);
                queue.push(i);
            }
        }
    }

    virtual Extent *getExtent() {
        if (sources[0].copier == NULL) {
            firstExtent();
        }

        while (true) {
            if (queue.empty()) {
                break;
            }
            uint32_t best = queue.top();

            LintelLogDebug("UnionModule/process", format("best was %d") % best);

            if (output_series.getExtent() == NULL) {
                output_series.setExtent(new Extent(*output_series.getType()));
            }

            output_series.newRecord();
            sources[best].copier->copyRecord();
            ++sources[best].series;
            if (!sources[best].series.more()) {
                delete sources[best].series.getExtent();
                sources[best].series.setExtent(sources[best].source->getExtent());
            }
            if (sources[best].series.getExtent() == NULL) {
                queue.pop();
            } else {
                queue.replaceTop(best); // it's position may have changed, re-add
            }
            if (output_series.getExtent()->size() > 96*1024) {
                break;
            }
        }

        return returnOutputSeries();
    }

    ExtentSeries output_series;
    vector<UM_UnionTable> sources; 
    Compare compare;
    PriorityQueue<uint32_t, Compare> queue;
    vector<string> order_columns;
    const string output_table_name;
};

/* Note: there are several versions of sort modules on the tomer sub-branch.  There is an in-memory
   sort, a radix in memory sort, a spilling to disk sort and a parallel sort.  The latter versions
   are somewhat specific to the sorting rules needed for the sort benchmark.  The former is a
   complicated template, and does not support sorting based on multiple columns.  Since the intent
   in this code is to first make slow, generalfield implementations and then eventually dynamically
   generate C++ source code for specific operations, we don't need the complication of the
   template, and so we create yet another module to be more in the style of the server */

#if 0
#include <algorithm>
#include <vector>

// Note the STL algorithm chooses to copy the comparator; the following program will abort on some
// (most/all?) STL implementations, which means that implementing a comparator with state is
// inherently less efficient unless the compilers value propagation does a really good job.

using namespace std;

class Comparator {
public:
    Comparator() { }

    Comparator(const Comparator &) { abort(); }

    Comparator &operator =(const Comparator &) { abort(); }
    
    bool operator ()(const int a, const int b) { return a < b; }
};

int main() {
    vector<int> a;
    a.push_back(1);
    a.push_back(1);
    a.push_back(1);
    a.push_back(1);
    a.push_back(1);

    sort(a.begin(), a.end(), Comparator());
}
#endif

class SortModule : public DataSeriesModule {
public:
    SortModule(DataSeriesModule &source, const vector<SortColumn> &sort_by)
        : source(source), sort_by(sort_by), copier(input_series, output_series), ercs(),
          sorted_extents(), merge()
    { }

    virtual ~SortModule() { }

    Extent *returnOutputSeries() {
        Extent *ret = output_series.getExtent();
        output_series.clearExtent();
        return ret;
    }

    struct SortColumnImpl {
        SortColumnImpl(GeneralField::Ptr field, bool sort_less)
            : field(field), sort_less(sort_less) { }
        GeneralField::Ptr field;
        bool sort_less; // a < b ==> sort_less
    };

    struct ExtentRowCompareState {
        ExtentRowCompareState() : extent(NULL), columns() { }
        const Extent *extent;
        vector<SortColumnImpl> columns;
    };

    static bool strictlyLessThan(const Extent *ea, const SEP_RowOffset &oa,
                                 const Extent *eb, const SEP_RowOffset &ob,
                                 const vector<SortColumnImpl> &columns) {
            BOOST_FOREACH(const SortColumnImpl &c, columns) {
                GeneralValue va(c.field->val(*ea, oa));
                GeneralValue vb(c.field->val(*eb, ob));
                if (va < vb) {
                    return c.sort_less;
                } else if (vb < va) {
                    return !c.sort_less;
                } else {
                    // ==; keep going
                }
            }
            return false; // all == so not <
    }

    class ExtentRowCompare {
    public:
        ExtentRowCompare(ExtentRowCompareState *state) : state(state) { }
        ExtentRowCompare(const ExtentRowCompare &from) : state(from.state) { }
        ExtentRowCompare &operator =(const ExtentRowCompare &rhs) {
            state = rhs.state;
            return *this;
        }

        bool operator ()(const SEP_RowOffset &a, const SEP_RowOffset &b) const {
            return strictlyLessThan(state->extent, a, state->extent, b, state->columns);
        }

        const ExtentRowCompareState *state;
    };
        
    struct SortedExtent {
        SortedExtent(Extent *e) : e(e), offsets(), pos() { }
        typedef boost::shared_ptr<SortedExtent> Ptr;

        Extent *e;
        vector<SEP_RowOffset> offsets;
        vector<SEP_RowOffset>::iterator pos;
    };
    
    struct LoserTreeCompare {
        LoserTreeCompare(SortModule *sm) : sm(sm) { }
        bool operator()(uint32_t ia, uint32_t ib) const {
            const SortedExtent &sea(*sm->sorted_extents[ia]);
            const SortedExtent &seb(*sm->sorted_extents[ib]);
            return strictlyLessThan(sea.e, *sea.pos, seb.e, *seb.pos, sm->ercs.columns);
        }

        const SortModule *sm;
    };        

    typedef __gnu_parallel::LoserTree<true, uint32_t, LoserTreeCompare> LoserTree;

    void firstExtent(Extent &in) {
        const ExtentType &t(in.getType());
        input_series.setType(t);
        output_series.setType(t);

        copier.prep();

        BOOST_FOREACH(SortColumn &by, sort_by) {
            TINVARIANT(by.sort_mode == SM_Ascending || by.sort_mode == SM_Decending);
            ercs.columns.push_back(SortColumnImpl(GeneralField::make(input_series, by.column),
                                                  by.sort_mode == SM_Ascending ? true : false));
        }
    }

    void sortExtent(Extent &in) {
        ercs.extent = &in;
        SortedExtent::Ptr se(new SortedExtent(&in));
        se->offsets.reserve(in.nRecords());
        for (input_series.setExtent(in); input_series.more(); input_series.next()) {
            se->offsets.push_back(input_series.getRowOffset());
        }

        stable_sort(se->offsets.begin(), se->offsets.end(), ExtentRowCompare(&ercs));

        BOOST_FOREACH(SEP_RowOffset &o, se->offsets) {
            GeneralValue v(ercs.columns[0].field->val(in, o));
        }
        ercs.extent = NULL;
        sorted_extents.push_back(se);
    }

    Extent *processSingleMerge() {
        output_series.setExtent(new Extent(*output_series.getType()));
        // loser tree gets this case wrong, and goes into an infinite loop (log_2(0) is a bad
        // idea with a check 0 * 2^n > 0.
        SortedExtent &se(*sorted_extents[0]);
        for (se.pos = se.offsets.begin(); se.pos != se.offsets.end(); ++se.pos) {
            output_series.newRecord();
            copier.copyRecord(*se.e, *se.pos);
        }
        delete se.e;
        sorted_extents.clear();
        return returnOutputSeries();
    }

    Extent *cleanupMultiMerge() {
        BOOST_FOREACH(SortedExtent::Ptr p, sorted_extents) {
            SINVARIANT(p->pos == p->offsets.end());
            delete p->e;
        }
        sorted_extents.clear();
        return returnOutputSeries();
    }

    Extent *processMerge() {
        LintelLogDebug("SortModule", "doing multi-extent-merge");
        output_series.setExtent(new Extent(*output_series.getType()));
        while (true) {
            int min = merge->get_min_source();
            if (min < 0 || static_cast<size_t>(min) >= sorted_extents.size()) {
                // loser tree exit path 1
                return cleanupMultiMerge();
            }
            SortedExtent &se(*sorted_extents[min]);
            if (se.pos == se.offsets.end()) {
                // loser tree exit path 2
                return cleanupMultiMerge();
            }

            output_series.newRecord();
            copier.copyRecord(*se.e, *se.pos);
            ++se.pos;
            merge->delete_min_insert(min, se.pos == se.offsets.end());
            if (output_series.getExtent()->size() > 96*1024) {
                break;
            }
        }
        return returnOutputSeries();
    }

    virtual Extent *getExtent() {
        if (sorted_extents.empty()) {
            while (true) {
                Extent *in = source.getExtent();
                if (in == NULL) {
                    break;
                }
                if (input_series.getType() == NULL) {
                    firstExtent(*in);
                }
                
                sortExtent(*in);
            }

            if (sorted_extents.empty()) {
                return NULL;
            } else if (sorted_extents.size() == 1) {
                return processSingleMerge();
            } else {
                merge = new LoserTree(sorted_extents.size(), LoserTreeCompare(this));

                for (uint32_t i = 0; i < sorted_extents.size(); ++i) {
                    merge->insert_start(i, i, false);
                    sorted_extents[i]->pos = sorted_extents[i]->offsets.begin();
                }
                merge->init();
            }
        } 

        return processMerge();
    }
        
    DataSeriesModule &source;
    vector<SortColumn> sort_by;
    ExtentSeries input_series, output_series;
    ExtentRecordCopy copier;
    ExtentRowCompareState ercs;
    vector<SortedExtent::Ptr> sorted_extents;
    LoserTree *merge;
};

class DataSeriesServerHandler : public DataSeriesServerIf, public ThrowError {
public:
    struct TableInfo {
	const ExtentType *extent_type;
	vector<string> depends_on;
	Clock::Tfrac last_update;
	TableInfo() : extent_type(), last_update(0) { }
    };

    typedef HashMap<string, TableInfo> NameToInfo;

    DataSeriesServerHandler() { };

    void ping() {
	LintelLog::info("ping()");
    }

    void shutdown() {
	LintelLog::info("shutdown()");
        exit(0);
    }

    bool hasTable(const string &table_name) {
        try {
            getTableInfo(table_name);
            return true;
        } catch (TException &e) {
            return false;
        }
    }

    void importDataSeriesFiles(const vector<string> &source_paths, const string &extent_type, 
			       const string &dest_table) {
	verifyTableName(dest_table);
	if (extent_type.empty()) {
	    requestError("extent type empty");
	}

	TypeIndexModule input(extent_type);
	TeeModule tee_op(input, tableToPath(dest_table));
	BOOST_FOREACH(const string &path, source_paths) {
	    input.addSource(path);
	}
	tee_op.getAndDelete();
	TableInfo &ti(table_info[dest_table]);
	ti.extent_type = input.getType();
	ti.last_update = Clock::todTfrac();
    }

    void importCSVFiles(const vector<string> &source_paths, const string &xml_desc, 
                        const string &dest_table, const string &field_separator,
                        const string &comment_prefix) {
	if (source_paths.empty()) {
	    requestError("missing source paths");
	}
        if (source_paths.size() > 1) {
            requestError("only supporting single insert");
        }
	verifyTableName(dest_table);
        pid_t pid = fork();
        if (pid < 0) {
            requestError("fork failed");
        } else if (pid == 0) {
            string xml_desc_path(str(format("xmldesc.%s") % dest_table));
            ofstream xml_desc_output(xml_desc_path.c_str());
            xml_desc_output << xml_desc;
            xml_desc_output.close();
            SINVARIANT(xml_desc_output.good());

            vector<string> args;
            args.push_back("csv2ds");
            args.push_back(str(format("--xml-desc-file=%s") % xml_desc_path));
            args.push_back(str(format("--field-separator=%s") % field_separator));
            args.push_back(str(format("--comment-prefix=%s") % comment_prefix));
            SINVARIANT(source_paths.size() == 1);
            copy(source_paths.begin(), source_paths.end(), back_inserter(args));
            args.push_back(tableToPath(dest_table));
            unlink(tableToPath(dest_table).c_str()); // ignore errors

            exec(args);
        } else {
            waitForSuccessfulChild(pid);

            ExtentTypeLibrary lib;
            const ExtentType &type(lib.registerTypeR(xml_desc));
            updateTableInfo(dest_table, &type);
        }
    }

    void importSQLTable(const string &dsn, const string &src_table, const string &dest_table) {
        verifyTableName(dest_table);

        pid_t pid = fork();
        if (pid < 0) {
            requestError("fork failed");
        } else if (pid == 0) {
            for(int i = 3; i < 100; ++i) {
                close(i);
            }
            vector<string> args;

            args.push_back("sql2ds");
            if (!dsn.empty()) {
                args.push_back(str(format("--dsn=%s") % dsn));
            }
            args.push_back(src_table);
            args.push_back(tableToPath(dest_table));
            exec(args);
        } else {
            waitForSuccessfulChild(pid);
            DataSeriesSource source(tableToPath(dest_table));
            const ExtentType *t = source.getLibrary().getTypeByName(src_table);

            updateTableInfo(dest_table, t); 
        }
    }
        
    void importData(const string &dest_table, const string &xml_desc, const TableData &data) {
        verifyTableName(dest_table);

        if (data.more_rows) {
            requestError("can not handle more rows");
        }
        ExtentTypeLibrary lib;
        const ExtentType &type(lib.registerTypeR(xml_desc));

        ExtentSeries output_series(type);
        DataSeriesSink output_sink(tableToPath(dest_table), Extent::compress_lzf, 1);
        OutputModule output_module(output_sink, output_series, type, 96*1024);

        output_sink.writeExtentLibrary(lib);

        vector<boost::shared_ptr<GeneralField> > fields;
        for (uint32_t i = 0; i < type.getNFields(); ++i) {
            GeneralField::Ptr tmp(GeneralField::make(output_series, type.getFieldName(i)));
            fields.push_back(tmp);
        }

        BOOST_FOREACH(const vector<string> &row, data.rows) {
            output_module.newRecord();
            if (row.size() != fields.size()) {
                requestError("incorrect number of fields");
            }
            for (uint32_t i = 0; i < row.size(); ++i) {
                fields[i]->set(row[i]);
            }
        }

        updateTableInfo(dest_table, &type);
    }

    void mergeTables(const vector<string> &source_tables, const string &dest_table) {
	if (source_tables.empty()) {
	    requestError("missing source tables");
	}
	verifyTableName(dest_table);
	vector<string> input_paths;
	input_paths.reserve(source_tables.size());
	string source_extent_type;
	BOOST_FOREACH(const string &table, source_tables) {
	    if (table == dest_table) {
		invalidTableName(table, "duplicated with destination table");
	    }
	    TableInfo *ti = table_info.lookup(table);
	    if (ti == NULL) {
		invalidTableName(table, "table not present");
	    }
	    if (source_extent_type.empty()) {
		source_extent_type = ti->extent_type->getName();
	    }
	    if (source_extent_type != ti->extent_type->getName()) {
		invalidTableName(table, str(format("extent type '%s' does not match earlier table"
                                                   " types of '%s'")
                                            % ti->extent_type % source_extent_type));
	    }
				       
	    input_paths.push_back(tableToPath(table));
	}
	if (source_extent_type.empty()) {
	    requestError("internal: extent type is missing?");
	}
	importDataSeriesFiles(input_paths, source_extent_type, dest_table);
    }

    void getTableData(TableData &ret, const string &source_table, int32_t max_rows, 
                      const string &where_expr) {
        verifyTableName(source_table);
        if (max_rows <= 0) {
            requestError("max_rows must be > 0");
        }
        NameToInfo::iterator i = getTableInfo(source_table);

        TypeIndexModule input(i->second.extent_type->getName());
        input.addSource(tableToPath(source_table));
        DataSeriesModule *mod = &input;
        boost::scoped_ptr<DataSeriesModule> select_module;
        if (!where_expr.empty()) {
            select_module.reset(new SelectModule(input, where_expr));
            mod = select_module.get();
        }

        TableDataModule sink(*mod, ret, max_rows);

        sink.getAndDelete();
    }

    void hashJoin(const string &a_table, const string &b_table, const string &out_table,
                  const map<string, string> &eq_columns, 
                  const map<string, string> &keep_columns, int32_t max_a_rows) { 
        NameToInfo::iterator a_info = getTableInfo(a_table);
        NameToInfo::iterator b_info = getTableInfo(b_table);

        verifyTableName(out_table);

        TypeIndexModule a_input(a_info->second.extent_type->getName());
        a_input.addSource(tableToPath(a_table));
        TypeIndexModule b_input(b_info->second.extent_type->getName());
        b_input.addSource(tableToPath(b_table));

        HashJoinModule hj_module(a_input, max_a_rows, b_input, eq_columns, keep_columns,
                                 out_table);

        TeeModule output_module(hj_module, tableToPath(out_table));
        
        output_module.getAndDelete();
        updateTableInfo(out_table, hj_module.output_series.getType());
    }

    void starJoin(const string &fact_table, const vector<Dimension> &dimensions, 
                  const string &out_table, const map<string, string> &fact_columns,
                  const vector<DimensionFactJoin> &dimension_columns, int32_t max_dimension_rows) {
        NameToInfo::iterator fact_info = getTableInfo(fact_table);
        verifyTableName(out_table);
        
        HashMap< string, shared_ptr<DataSeriesModule> > dimension_modules;
        
        BOOST_FOREACH(const Dimension &dim, dimensions) {
            if (!dimension_modules.exists(dim.source_table)) {
                NameToInfo::iterator dim_info = getTableInfo(dim.source_table);
                shared_ptr<TypeIndexModule> 
                    ptr(new TypeIndexModule(dim_info->second.extent_type->getName()));
                ptr->addSource(tableToPath(dim.source_table));
                dimension_modules[dim.source_table] = ptr;
            }
        }
        
        TypeIndexModule fact_input(fact_info->second.extent_type->getName());
        fact_input.addSource(tableToPath(fact_table));

        // TODO: use and check max_dimension_rows
        StarJoinModule sj_module(fact_input, dimensions, out_table, fact_columns, 
                                 dimension_columns, dimension_modules);

        TeeModule output_module(sj_module, tableToPath(out_table));

        output_module.getAndDelete();
        updateTableInfo(out_table, sj_module.output_series.getType());
    }
    
    void selectRows(const string &in_table, const string &out_table, const string &where_expr) {
        verifyTableName(in_table);
        verifyTableName(out_table);
        NameToInfo::iterator info = getTableInfo(in_table);
        TypeIndexModule input(info->second.extent_type->getName());
        input.addSource(tableToPath(in_table));
        SelectModule select(input, where_expr);
        TeeModule output_module(select, tableToPath(out_table));

        output_module.getAndDelete();
        updateTableInfo(out_table, info->second.extent_type);
    }

    void projectTable(const string &in_table, const string &out_table, 
                      const vector<string> &keep_columns) {
        verifyTableName(in_table);
        verifyTableName(out_table);

        NameToInfo::iterator info = getTableInfo(in_table);
        TypeIndexModule input(info->second.extent_type->getName());
        input.addSource(tableToPath(in_table));
        ProjectModule project(input, keep_columns);
        TeeModule output_module(project, tableToPath(out_table));
        output_module.getAndDelete();
        updateTableInfo(out_table, project.output_series.getType());
    }

    void sortedUpdateTable(const string &base_table, const string &update_from, 
                           const string &update_column, const vector<string> &primary_key) {
        verifyTableName(base_table);
        verifyTableName(update_from);

        NameToInfo::iterator update_info(getTableInfo(update_from));
        NameToInfo::iterator base_info = table_info.find(base_table);
        if (base_info == table_info.end()) {
            base_info = createTable(base_table, update_info, update_column);
        }

        TypeIndexModule base_input(base_info->second.extent_type->getName());
        base_input.addSource(tableToPath(base_table));

        TypeIndexModule update_input(update_info->second.extent_type->getName());
        update_input.addSource(tableToPath(update_from));

        SortedUpdateModule updater(base_input, update_input, update_column, primary_key);

        TeeModule output_module(updater, tableToPath(base_table, "tmp."));
        output_module.getAndDelete();
        output_module.close();
        string from(tableToPath(base_table, "tmp.")), to(tableToPath(base_table));
        int ret = rename(from.c_str(), to.c_str());
        INVARIANT(ret == 0, format("rename %s -> %s failed: %s") % from % to % strerror(errno));
    }

    void unionTables(const vector<UnionTable> &in_tables, const vector<string> &order_columns,
                     const string &out_table) {
        typedef UnionModule::UM_UnionTable UM_UnionTable;
        
        vector<UM_UnionTable> tables;
        BOOST_FOREACH(const UnionTable &table, in_tables) {
            verifyTableName(table.table_name);
            NameToInfo::iterator table_info(getTableInfo(table.table_name));

            TypeIndexModule::Ptr p =
                TypeIndexModule::make(table_info->second.extent_type->getName());
            p->addSource(tableToPath(table_info->first));
            tables.push_back(UM_UnionTable(table, p));
        }
        
        UnionModule union_mod(tables, order_columns, out_table);
        TeeModule output_module(union_mod, tableToPath(out_table));

        output_module.getAndDelete();
        updateTableInfo(out_table, union_mod.output_series.getType());
    }

    void sortTable(const string &in_table, const string &out_table, const vector<SortColumn> &by) {
        NameToInfo::iterator table_info(getTableInfo(in_table));
        
        TypeIndexModule::Ptr p(TypeIndexModule::make(table_info->second.extent_type->getName()));
        p->addSource(tableToPath(table_info->first));

        SortModule sorter(*p, by);
        
        TeeModule output_module(sorter, tableToPath(out_table));
        output_module.getAndDelete();
        updateTableInfo(out_table, sorter.output_series.getType());
    }

private:
    void verifyTableName(const string &name) {
	if (name.size() >= 200) {
	    invalidTableName(name, "name too long");
	}
	if (name.find('/') != string::npos) {
	    invalidTableName(name, "contains /");
	}
    }

    string tableToPath(const string &table_name, const string &prefix = "ds.") {
        verifyTableName(table_name);
        return prefix + table_name;
    }

    void updateTableInfo(const string &table, const ExtentType *extent_type) {
        TableInfo &info(table_info[table]);
        info.extent_type = extent_type;
    }

    void waitForSuccessfulChild(pid_t pid) {
        int status = -1;
        if (waitpid(pid, &status, 0) != pid) {
            requestError("waitpid() failed");
        }
        if (WEXITSTATUS(status) != 0) {
            requestError("csv2ds failed");
        }
    }

    NameToInfo::iterator 
    createTable(const string &table_name, NameToInfo::iterator update_table, 
                const std::string &update_column) {
        const ExtentType *from_type = update_table->second.extent_type;
        string extent_type = str(format("<ExtentType name=\"%s\" namespace=\"%s\""
                                        " version=\"%d.%d\">") % table_name 
                                 % from_type->getNamespace() % from_type->majorVersion()
                                 % from_type->minorVersion());

        for (uint32_t i = 0; i < from_type->getNFields(); ++i) {
            string field_name = from_type->getFieldName(i);
            if (field_name == update_column) {
                continue; // ignore
            }
            extent_type.append(from_type->xmlFieldDesc(field_name));
        }
        extent_type.append("</ExtentType>");

        DataSeriesSink output(tableToPath(table_name), Extent::compress_lzf, 1);
        ExtentTypeLibrary library;
        const ExtentType &type(library.registerTypeR(extent_type));
        output.writeExtentLibrary(library);
        Extent tmp(type);
        output.writeExtent(tmp, NULL);
        output.close();
        updateTableInfo(table_name, library.getTypeByName(table_name));
        return getTableInfo(table_name);
    }

    void exec(vector<string> &args) {
        char **argv = new char *[args.size() + 1];
        const char *tmp;
        for (uint32_t i = 0; i < args.size(); ++i) {
            tmp = args[i].c_str(); // force null termination
            argv[i] = &args[i][0]; // couldn't figure out how to directly use c_str()
        }
        argv[args.size()] = NULL;
        execvp(args[0].c_str(), argv);
        FATAL_ERROR(format("exec of %s failed: %s") % args[0] % strerror(errno));
    }

    NameToInfo::iterator getTableInfo(const string &table_name) {
        NameToInfo::iterator ret = table_info.find(table_name);
        if (ret == table_info.end()) {
            invalidTableName(table_name, "table missing");
        }
        return ret;
    }

    NameToInfo table_info;
};

lintel::ProgramOption<string> po_working_directory
("working-directory", "Specifies the working directory for cached intermediate tables");

void setupWorkingDirectory() {
    string working_directory = po_working_directory.get();
    if (!po_working_directory.used()) {
	working_directory = "/tmp/ds-server.";
	struct passwd *p = getpwuid(getuid());
	SINVARIANT(p != NULL);
	working_directory += p->pw_name;
    }

    struct stat stat_buf;
    int ret = stat(working_directory.c_str(), &stat_buf);
    CHECKED((ret == -1 && errno == ENOENT) || (ret == 0 && S_ISDIR(stat_buf.st_mode)),
	    format("Error accessing %s: %s") % working_directory
	    % (ret == 0 ? "not a directory" : strerror(errno)));
    if (ret == -1 && errno == ENOENT) {
	CHECKED(mkdir(working_directory.c_str(), 0777) == 0,
		format("Unable to create directory %s: %s") % working_directory % strerror(errno));
    }
    CHECKED(chdir(working_directory.c_str()) == 0,
	    format("Unable to chdir to %s: %s") % working_directory % strerror(errno));
}

int main(int argc, char *argv[]) {
    LintelLog::parseEnv();
    shared_ptr<TProtocolFactory> protocolFactory(new TBinaryProtocolFactory());
    shared_ptr<DataSeriesServerHandler> handler(new DataSeriesServerHandler());
    shared_ptr<TProcessor> processor(new DataSeriesServerProcessor(handler));
    shared_ptr<TServerTransport> serverTransport(new TServerSocket(49476));
    shared_ptr<TTransportFactory> transportFactory(new TBufferedTransportFactory());

    TSimpleServer server(processor, serverTransport, transportFactory, protocolFactory);

    setupWorkingDirectory();

    LintelLog::info("start...");
    server.serve();
    LintelLog::info("finish.");

    return 0;
}