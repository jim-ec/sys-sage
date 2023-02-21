#include "sys-sage.hpp"

#include "gtest/gtest.h"
#include "gmock/gmock.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlschemas.h>
#include <libxml/xpath.h>

#include <string_view>

int main(int argc, char **argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

/**
 * Create a string view over UTF-8 code points as used by libxml.
 */
std::basic_string_view<const xmlChar> operator""_xsv(const char *string, size_t len)
{
    return {BAD_CAST(string), len};
}

/**
 * A container which implements RAII (Resource acquisition is initialization) for
 * automatic destructor dispatch.
 */
template <class T>
class RAII
{
    using Dtor = void (*)(T *);

    T *inner;  /** The acquired value. Is never null. */
    Dtor dtor; /** The destructor to run during de-initialization. */

public:
    /**
     * Constructs a new RAII object owning an inner value and an associated destructor.
     */
    RAII(T *inner, Dtor dtor)
        : inner{inner}, dtor{dtor}
    {
        if (inner == nullptr)
        {
            throw std::runtime_error{"Failed to acquire test resource"};
        }
    }

    /**
     * Sometimes, the destructor accepts a generic `void *` instead of the concrete value type.
     */
    RAII(T *inner, void (*dtor)(void *))
    requires(!std::is_same_v<T, void>)
        : inner{inner}, dtor{reinterpret_cast<Dtor>(dtor)}
    {
        if (inner == nullptr)
        {
            throw std::runtime_error{"Failed to acquire test resource"};
        }
    }

    RAII(const RAII &) = delete;

    /**
     * Transfer ownership.
     */
    RAII(RAII &&rhs) : inner{rhs.inner}, dtor{rhs.dtor}
    {
        rhs.dtor = nullptr;
    }

    RAII &operator=(const RAII &) = delete;

    RAII &operator=(RAII &&rhs)
    {
        inner = rhs.inner;
        dtor = rhs.dtor;
        rhs.dtor = nullptr;
    };

    ~RAII()
    {
        if (dtor != nullptr)
        {
            dtor(inner);
        }
    }

    T *operator->()
    {
        return inner;
    }

    operator T *()
    {
        return inner;
    }

    T *operator*()
    {
        return inner;
    }
};

template <class T, class D>
RAII(T *, D) -> RAII<T>;

template <class D>
RAII(void *, D) -> RAII<void>;

TEST(RAII, InnerCannotBeNull)
{
    ASSERT_ANY_THROW(RAII(reinterpret_cast<void *>(0), free));
    ASSERT_ANY_THROW(RAII(reinterpret_cast<int *>(0), [](void *) {}));
}

TEST(RAII, DestructorRuns)
{
    int x = 0;
    {
        RAII raii{&x, [](int *x)
                  { *x = 42; }};
        ASSERT_EQ(x, 0);
        ASSERT_EQ(*static_cast<int *>(raii), 0);
    }
    ASSERT_EQ(x, 42);
}

TEST(RAII, MoveOwnership)
{
    int x = 0;
    {
        RAII r0{&x, [](int *x)
                { *x += 1; }};
        RAII r1{std::move(r0)};
        ASSERT_EQ(x, 0);
        ASSERT_EQ(*static_cast<int *>(r1), 0);
    }
    ASSERT_EQ(x, 1);
}

/**
 * Validates XML output of sys-sage using an XML schema file.
 */
void validate(std::string_view path)
{
    auto parser = RAII{xmlSchemaNewParserCtxt(TEST_RESOURCE_DIR "/schema.xml"), xmlSchemaFreeParserCtxt};
    if (parser == nullptr)
    {
        throw std::runtime_error{""};
    }

    auto schema = RAII{xmlSchemaParse(parser), xmlSchemaFree};

    auto validator = RAII{xmlSchemaNewValidCtxt(schema), xmlSchemaFreeValidCtxt};
    if (validator == nullptr)
    {
        throw std::runtime_error{""};
    }

    xmlSchemaSetValidErrors(validator, (xmlSchemaValidityErrorFunc)fprintf, (xmlSchemaValidityWarningFunc)fprintf, stdout);

    auto doc = RAII{xmlParseFile(path.data()), xmlFreeDoc};
    if (doc == nullptr)
    {
        std::string message{"Cannot open "};
        message += path;
        message += " for validation";
        throw std::runtime_error{message};
    }

    int code = xmlSchemaValidateDoc(validator, doc);
    if (code != 0)
    {
        std::string message{"XML validation of "};
        message += path;
        message += " failed";
        throw std::runtime_error{message};
    }
}

TEST(ExportToXml, MinimalTopology)
{
    auto topo = new Component{42, "a name", SYS_SAGE_COMPONENT_NONE};
    exportToXml(topo, "test_export.xml");
    validate("test_export.xml");
}

xmlNode *getSingleNodeByPath(xmlChar *path, xmlXPathContext *context)
{
    auto result = RAII{xmlXPathEvalExpression(path, context), xmlXPathFreeObject};
    if (result->nodesetval == nullptr || result->type != XPATH_NODESET || result->nodesetval->nodeNr != 1)
    {
        std::string message{"Invalid query with path "};
        message += reinterpret_cast<const char *>(path);
        throw std::runtime_error{message};
    }
    return result->nodesetval->nodeTab[0];
}

TEST(ExportToXml, SingleComponent)
{
    {
        auto topo = new Topology;
        auto memory = new Memory{topo, "A single memory component", 16};
        (void)memory;
        exportToXml(topo, "test_export.xml");
    }

    {
        validate("test_export.xml");

        auto doc = RAII{xmlParseFile("test_export.xml"), xmlFreeDoc};
        auto pathContext = RAII{xmlXPathNewContext(doc), xmlXPathFreeContext};

        ASSERT_NO_THROW(getSingleNodeByPath(BAD_CAST("/sys-sage/components/Topology"), pathContext));

        {
            auto result = RAII{xmlXPathEvalExpression(BAD_CAST("string(/sys-sage/components/Topology/@id)"), pathContext), xmlXPathFreeObject};
            ASSERT_EQ("0"_xsv, result->stringval);
        }

        {
            auto result = RAII{xmlXPathEvalExpression(BAD_CAST("string(/sys-sage/components/Topology/@name)"), pathContext), xmlXPathFreeObject};
            ASSERT_EQ("sys-sage Topology"_xsv, result->stringval);
        }

        ASSERT_NO_THROW(getSingleNodeByPath(BAD_CAST("/sys-sage/components/Topology/Memory"), pathContext));

        {
            auto result = RAII{xmlXPathEvalExpression(BAD_CAST("string(/sys-sage/components/Topology/Memory/@id)"), pathContext), xmlXPathFreeObject};
            ASSERT_EQ("0"_xsv, result->stringval);
        }

        {
            auto result = RAII{xmlXPathEvalExpression(BAD_CAST("string(/sys-sage/components/Topology/Memory/@name)"), pathContext), xmlXPathFreeObject};
            ASSERT_EQ("A single memory component"_xsv, result->stringval);
        }

        {
            auto result = RAII{xmlXPathEvalExpression(BAD_CAST("string(/sys-sage/components/Topology/Memory/@size)"), pathContext), xmlXPathFreeObject};
            ASSERT_EQ("16"_xsv, result->stringval);
        }
    }
}