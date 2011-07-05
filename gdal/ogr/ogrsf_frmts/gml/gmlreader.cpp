/******************************************************************************
 * $Id$
 *
 * Project:  GML Reader
 * Purpose:  Implementation of GMLReader class.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2002, Frank Warmerdam
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "gmlreader.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "gmlutils.h"
#include "gmlreaderp.h"
#include "cpl_conv.h"

#define SUPPORT_GEOMETRY

#ifdef SUPPORT_GEOMETRY
#  include "ogr_geometry.h"
#endif

/************************************************************************/
/*                            ~IGMLReader()                             */
/************************************************************************/

IGMLReader::~IGMLReader()

{
}

/************************************************************************/
/* ==================================================================== */
/*                  No XERCES or EXPAT Library                          */
/* ==================================================================== */
/************************************************************************/
#if !defined(HAVE_XERCES) && !defined(HAVE_EXPAT)

/************************************************************************/
/*                          CreateGMLReader()                           */
/************************************************************************/

IGMLReader *CreateGMLReader(int bUseExpatParserPreferably,
                            int bInvertAxisOrderIfLatLong,
                            int bConsiderEPSGAsURN)

{
    CPLError( CE_Failure, CPLE_AppDefined,
              "Unable to create Xerces C++ or Expat based GML reader, Xerces or Expat support\n"
              "not configured into GDAL/OGR." );
    return NULL;
}

/************************************************************************/
/* ==================================================================== */
/*                  With XERCES or EXPAT Library                        */
/* ==================================================================== */
/************************************************************************/
#else /* defined(HAVE_XERCES) || defined(HAVE_EXPAT) */

/************************************************************************/
/*                          CreateGMLReader()                           */
/************************************************************************/

IGMLReader *CreateGMLReader(int bUseExpatParserPreferably,
                            int bInvertAxisOrderIfLatLong,
                            int bConsiderEPSGAsURN)

{
    return new GMLReader(bUseExpatParserPreferably,
                         bInvertAxisOrderIfLatLong,
                         bConsiderEPSGAsURN);
}

#endif

int GMLReader::m_bXercesInitialized = FALSE;
int GMLReader::m_nInstanceCount = 0;

/************************************************************************/
/*                             GMLReader()                              */
/************************************************************************/

GMLReader::GMLReader(int bUseExpatParserPreferably, int bInvertAxisOrderIfLatLong, int bConsiderEPSGAsURN)

{
#ifndef HAVE_XERCES
    bUseExpatReader = TRUE;
#else
    bUseExpatReader = FALSE;
#ifdef HAVE_EXPAT
    if(bUseExpatParserPreferably)
        bUseExpatReader = TRUE;
#endif
#endif

#if defined(HAVE_EXPAT) && defined(HAVE_XERCES)
    if (bUseExpatReader)
        CPLDebug("GML", "Using Expat reader");
    else
        CPLDebug("GML", "Using Xerces reader");
#endif

    m_nInstanceCount++;
    m_nClassCount = 0;
    m_papoClass = NULL;

    m_bClassListLocked = FALSE;

    m_poGMLHandler = NULL;
#ifdef HAVE_XERCES
    m_poSAXReader = NULL;
    m_poCompleteFeature = NULL;
    m_GMLInputSource = NULL;
#endif
#ifdef HAVE_EXPAT
    oParser = NULL;
    ppoFeatureTab = NULL;
    nFeatureTabIndex = 0;
    nFeatureTabLength = 0;
#endif
    fpGML = NULL;
    m_bReadStarted = FALSE;
    
    m_poState = NULL;

    m_pszFilename = NULL;

    m_bStopParsing = FALSE;

    /* A bit experimental. Not publicly advertized. See commented doc in drv_gml.html */
    m_bFetchAllGeometries = CSLTestBoolean(CPLGetConfigOption("GML_FETCH_ALL_GEOMETRIES", "NO"));

    m_bInvertAxisOrderIfLatLong = bInvertAxisOrderIfLatLong;
    m_bConsiderEPSGAsURN = bConsiderEPSGAsURN;

    m_pszGlobalSRSName = NULL;
    m_bCanUseGlobalSRSName = FALSE;

    m_pszFilteredClassName = NULL;
}

/************************************************************************/
/*                             ~GMLReader()                             */
/************************************************************************/

GMLReader::~GMLReader()

{
    ClearClasses();

    CPLFree( m_pszFilename );

    CleanupParser();

    --m_nInstanceCount;
#ifdef HAVE_XERCES
    if( m_nInstanceCount == 0 && m_bXercesInitialized )
    {
        XMLPlatformUtils::Terminate();
        m_bXercesInitialized = FALSE;
    }
#endif

    if (fpGML)
        VSIFCloseL(fpGML);
    fpGML = NULL;

    CPLFree(m_pszGlobalSRSName);

    CPLFree(m_pszFilteredClassName);
}

/************************************************************************/
/*                          SetSourceFile()                             */
/************************************************************************/

void GMLReader::SetSourceFile( const char *pszFilename )

{
    CPLFree( m_pszFilename );
    m_pszFilename = CPLStrdup( pszFilename );
}

/************************************************************************/
/*                       GetSourceFileName()                           */
/************************************************************************/

const char* GMLReader::GetSourceFileName()

{
    return m_pszFilename;
}

#ifdef HAVE_EXPAT

static void XMLCALL startElementCbk(void *pUserData, const char *pszName,
                                    const char **ppszAttr)
{
    ((GMLHandler*)pUserData)->startElement(pszName, ppszAttr);
}

static void XMLCALL endElementCbk(void *pUserData, const char *pszName)
{
    ((GMLHandler*)pUserData)->endElement(pszName);
}

static void XMLCALL dataHandlerCbk(void *pUserData, const char *data, int nLen)
{
    ((GMLHandler*)pUserData)->dataHandler(data, nLen);
}

#endif

/************************************************************************/
/*                            SetupParser()                             */
/************************************************************************/

int GMLReader::SetupParser()

{
    if (fpGML == NULL)
        fpGML = VSIFOpenL(m_pszFilename, "rt");
    if (fpGML != NULL)
        VSIFSeekL( fpGML, 0, SEEK_SET );

    int bRet = -1;
#ifdef HAVE_EXPAT
    if (bUseExpatReader)
        bRet = SetupParserExpat();
#endif

#ifdef HAVE_XERCES
    if (!bUseExpatReader)
        bRet = SetupParserXerces();
#endif
    if (bRet < 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "SetupParser(): shouldn't happen");
        return FALSE;
    }

    if (!bRet)
        return FALSE;

    m_bReadStarted = FALSE;

    // Push an empty state.
    PushState( new GMLReadState() );

    return TRUE;
}

#ifdef HAVE_XERCES
/************************************************************************/
/*                        SetupParserXerces()                           */
/************************************************************************/

int GMLReader::SetupParserXerces()
{
    if( !m_bXercesInitialized )
    {
        try
        {
            XMLPlatformUtils::Initialize();
        }
        
        catch (const XMLException& toCatch)
        {
            CPLError( CE_Warning, CPLE_AppDefined,
                      "Exception initializing Xerces based GML reader.\n%s", 
                      tr_strdup(toCatch.getMessage()) );
            return FALSE;
        }
        m_bXercesInitialized = TRUE;
    }

    // Cleanup any old parser.
    if( m_poSAXReader != NULL )
        CleanupParser();

    // Create and initialize parser.
    XMLCh* xmlUriValid = NULL;
    XMLCh* xmlUriNS = NULL;

    try{
        m_poSAXReader = XMLReaderFactory::createXMLReader();
    
        GMLXercesHandler* poXercesHandler = new GMLXercesHandler( this );
        m_poGMLHandler = poXercesHandler;

        m_poSAXReader->setContentHandler( poXercesHandler );
        m_poSAXReader->setErrorHandler( poXercesHandler );
        m_poSAXReader->setLexicalHandler( poXercesHandler );
        m_poSAXReader->setEntityResolver( poXercesHandler );
        m_poSAXReader->setDTDHandler( poXercesHandler );

        xmlUriValid = XMLString::transcode("http://xml.org/sax/features/validation");
        xmlUriNS = XMLString::transcode("http://xml.org/sax/features/namespaces");

#if (OGR_GML_VALIDATION)
        m_poSAXReader->setFeature( xmlUriValid, true);
        m_poSAXReader->setFeature( xmlUriNS, true);

        m_poSAXReader->setFeature( XMLUni::fgSAX2CoreNameSpaces, true );
        m_poSAXReader->setFeature( XMLUni::fgXercesSchema, true );

//    m_poSAXReader->setDoSchema(true);
//    m_poSAXReader->setValidationSchemaFullChecking(true);
#else
        m_poSAXReader->setFeature( XMLUni::fgSAX2CoreValidation, false);

#if XERCES_VERSION_MAJOR >= 3
        m_poSAXReader->setFeature( XMLUni::fgXercesSchema, false);
#else
        m_poSAXReader->setFeature( XMLUni::fgSAX2CoreNameSpaces, false);
#endif

#endif
        XMLString::release( &xmlUriValid );
        XMLString::release( &xmlUriNS );
    }
    catch (...)
    {
        XMLString::release( &xmlUriValid );
        XMLString::release( &xmlUriNS );

        CPLError( CE_Warning, CPLE_AppDefined,
                  "Exception initializing Xerces based GML reader.\n" );
        return FALSE;
    }

    if (m_GMLInputSource == NULL && fpGML != NULL)
        m_GMLInputSource = new GMLInputSource(fpGML);

    return TRUE;
}
#endif

/************************************************************************/
/*                        SetupParserExpat()                            */
/************************************************************************/

#ifdef HAVE_EXPAT
int GMLReader::SetupParserExpat()
{
    // Cleanup any old parser.
    if( oParser != NULL )
        CleanupParser();

    oParser = OGRCreateExpatXMLParser();
    m_poGMLHandler = new GMLExpatHandler( this, oParser );

    XML_SetElementHandler(oParser, ::startElementCbk, ::endElementCbk);
    XML_SetCharacterDataHandler(oParser, ::dataHandlerCbk);
    XML_SetUserData(oParser, m_poGMLHandler);
    return TRUE;
}
#endif

/************************************************************************/
/*                           CleanupParser()                            */
/************************************************************************/

void GMLReader::CleanupParser()

{
#ifdef HAVE_XERCES
    if( !bUseExpatReader && m_poSAXReader == NULL )
        return;
#endif

#ifdef HAVE_EXPAT
    if ( bUseExpatReader && oParser == NULL )
        return;
#endif

    while( m_poState )
        PopState();

#ifdef HAVE_XERCES
    delete m_poSAXReader;
    m_poSAXReader = NULL;
    delete m_GMLInputSource;
    m_GMLInputSource = NULL;
    delete m_poCompleteFeature;
    m_poCompleteFeature = NULL;
#endif

#ifdef HAVE_EXPAT
    if (oParser)
        XML_ParserFree(oParser);
    oParser = NULL;

    int i;
    for(i=nFeatureTabIndex;i<nFeatureTabLength;i++)
        delete ppoFeatureTab[i];
    CPLFree(ppoFeatureTab);
    nFeatureTabIndex = 0;
    nFeatureTabLength = 0;
    ppoFeatureTab = NULL;

#endif

    delete m_poGMLHandler;
    m_poGMLHandler = NULL;

    m_bReadStarted = FALSE;
}

#ifdef HAVE_XERCES

GMLBinInputStream::GMLBinInputStream(VSILFILE* fp)
{
    this->fp = fp;
    emptyString = 0;
}

GMLBinInputStream::~ GMLBinInputStream()
{
}

#if XERCES_VERSION_MAJOR >= 3
XMLFilePos GMLBinInputStream::curPos() const
{
    return (XMLFilePos)VSIFTellL(fp);
}

XMLSize_t GMLBinInputStream::readBytes(XMLByte* const toFill, const XMLSize_t maxToRead)
{
    return (XMLSize_t)VSIFReadL(toFill, 1, maxToRead, fp);
}

const XMLCh* GMLBinInputStream::getContentType() const
{
    return &emptyString;
}
#else
unsigned int GMLBinInputStream::curPos() const
{
    return (unsigned int)VSIFTellL(fp);
}

unsigned int GMLBinInputStream::readBytes(XMLByte* const toFill, const unsigned int maxToRead)
{
    return (unsigned int)VSIFReadL(toFill, 1, maxToRead, fp);
}
#endif

GMLInputSource::GMLInputSource(VSILFILE* fp, MemoryManager* const manager) : InputSource(manager)
{
    binInputStream = new GMLBinInputStream(fp);
}

GMLInputSource::~GMLInputSource()
{
}

BinInputStream* GMLInputSource::makeStream() const
{
    return binInputStream;
}

#endif // HAVE_XERCES

/************************************************************************/
/*                        NextFeatureXerces()                           */
/************************************************************************/

#ifdef HAVE_XERCES
GMLFeature *GMLReader::NextFeatureXerces()

{
    GMLFeature *poReturn = NULL;

    try
    {
        if( !m_bReadStarted )
        {
            if( m_poSAXReader == NULL )
                SetupParser();

            m_bReadStarted = TRUE;

            if (m_GMLInputSource == NULL)
                return NULL;

            if( !m_poSAXReader->parseFirst( *m_GMLInputSource, m_oToFill ) )
                return NULL;
        }

        while( m_poCompleteFeature == NULL 
               && !m_bStopParsing
               && m_poSAXReader->parseNext( m_oToFill ) ) {}

        poReturn = m_poCompleteFeature;
        m_poCompleteFeature = NULL;

    }
    catch (const XMLException& toCatch)
    {
        char *pszErrorMessage = tr_strdup( toCatch.getMessage() );
        CPLDebug( "GML", 
                  "Error during NextFeature()! Message:\n%s", 
                  pszErrorMessage );
        CPLFree(pszErrorMessage);
        m_bStopParsing = TRUE;
    }
    catch (const SAXException& toCatch)
    {
        char *pszErrorMessage = tr_strdup( toCatch.getMessage() );
        CPLError(CE_Failure, CPLE_AppDefined, "%s", pszErrorMessage);
        CPLFree(pszErrorMessage);
        m_bStopParsing = TRUE;
    }

    return poReturn;
}
#endif

#ifdef HAVE_EXPAT
GMLFeature *GMLReader::NextFeatureExpat()

{
    if (!m_bReadStarted)
    {
        if (oParser == NULL)
            SetupParser();

        m_bReadStarted = TRUE;
    }

    if (fpGML == NULL || m_bStopParsing)
        return NULL;

    if (nFeatureTabIndex < nFeatureTabLength)
    {
        return ppoFeatureTab[nFeatureTabIndex++];
    }

    if (VSIFEofL(fpGML))
        return NULL;

    char aBuf[BUFSIZ];

    CPLFree(ppoFeatureTab);
    ppoFeatureTab = NULL;
    nFeatureTabLength = 0;
    nFeatureTabIndex = 0;

    int nDone;
    do
    {
        ((GMLExpatHandler*)m_poGMLHandler)->ResetDataHandlerCounter();

        unsigned int nLen =
                (unsigned int)VSIFReadL( aBuf, 1, sizeof(aBuf), fpGML );
        nDone = VSIFEofL(fpGML);
        if (XML_Parse(oParser, aBuf, nLen, nDone) == XML_STATUS_ERROR)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "XML parsing of GML file failed : %s "
                     "at line %d, column %d",
                     XML_ErrorString(XML_GetErrorCode(oParser)),
                     (int)XML_GetCurrentLineNumber(oParser),
                     (int)XML_GetCurrentColumnNumber(oParser));
            m_bStopParsing = TRUE;
        }
        if (!m_bStopParsing)
            m_bStopParsing = ((GMLExpatHandler*)m_poGMLHandler)->HasStoppedParsing();

    } while (!nDone && !m_bStopParsing && nFeatureTabLength == 0);

    return (nFeatureTabLength) ? ppoFeatureTab[nFeatureTabIndex++] : NULL;
}
#endif

GMLFeature *GMLReader::NextFeature()
{
#ifdef HAVE_EXPAT
    if (bUseExpatReader)
        return NextFeatureExpat();
#endif

#ifdef HAVE_XERCES
    if (!bUseExpatReader)
        return NextFeatureXerces();
#endif

    CPLError(CE_Failure, CPLE_AppDefined, "NextFeature(): Should not happen");
    return NULL;
}

/************************************************************************/
/*                            PushFeature()                             */
/*                                                                      */
/*      Create a feature based on the named element.  If the            */
/*      corresponding feature class doesn't exist yet, then create      */
/*      it now.  A new GMLReadState will be created for the feature,    */
/*      and it will be placed within that state.  The state is          */
/*      pushed onto the readstate stack.                                */
/************************************************************************/

void GMLReader::PushFeature( const char *pszElement, 
                             const char *pszFID )

{
    int iClass;

/* -------------------------------------------------------------------- */
/*      Find the class of this element.                                 */
/* -------------------------------------------------------------------- */
    for( iClass = 0; iClass < GetClassCount(); iClass++ )
    {
        if( EQUAL(pszElement,GetClass(iClass)->GetElementName()) )
            break;
    }

/* -------------------------------------------------------------------- */
/*      Create a new feature class for this element, if there is no     */
/*      existing class for it.                                          */
/* -------------------------------------------------------------------- */
    if( iClass == GetClassCount() )
    {
        CPLAssert( !IsClassListLocked() );

        GMLFeatureClass *poNewClass = new GMLFeatureClass( pszElement );

        AddClass( poNewClass );
    }

/* -------------------------------------------------------------------- */
/*      Create a feature of this feature class.  Try to set the fid     */
/*      if available.                                                   */
/* -------------------------------------------------------------------- */
    GMLFeature *poFeature = new GMLFeature( GetClass( iClass ) );
    if( pszFID != NULL )
    {
        poFeature->SetFID( pszFID );
    }

/* -------------------------------------------------------------------- */
/*      Create and push a new read state.                               */
/* -------------------------------------------------------------------- */
    GMLReadState *poState;

    poState = new GMLReadState();
    poState->m_poFeature = poFeature;
    PushState( poState );
}

/************************************************************************/
/*                          IsFeatureElement()                          */
/*                                                                      */
/*      Based on context and the element name, is this element a new    */
/*      GML feature element?                                            */
/************************************************************************/

int GMLReader::IsFeatureElement( const char *pszElement )

{
    CPLAssert( m_poState != NULL );

    const char *pszLast = m_poState->GetLastComponent();
    int        nLen = strlen(pszLast);
    int        nElementLength = strlen(pszElement);

    if (strcmp(pszLast, "dane") == 0)
    {
         /* Polish TBD GML */
    }

    /* Begin of OpenLS */
    else if (strcmp(pszLast, "GeocodeResponseList") == 0 &&
             strcmp(pszElement, "GeocodedAddress") == 0)
    {
    }
    else if (strcmp(pszLast, "DetermineRouteResponse") == 0)
    {
        /* We don't want the children of RouteInstructionsList */
        /* to be a single feature. We want each RouteInstruction */
        /* to be a feature */
        if (strcmp(pszElement, "RouteInstructionsList") == 0)
            return FALSE;
    }
    else if (strcmp(pszElement, "RouteInstruction") == 0 &&
             strcmp(pszLast, "RouteInstructionsList") == 0)
    {
    }
    /* End of OpenLS */

    else if (nLen > 6 && strcmp(pszLast + nLen - 6, "_layer") == 0 &&
             nElementLength > 8 && strcmp(pszElement + nElementLength - 8, "_feature") == 0)
    {
        /* GML answer of MapServer WMS GetFeatureInfo request */
    }

    else if( nLen < 6 || !(EQUAL(pszLast+nLen-6,"member") ||
                      EQUAL(pszLast+nLen-7,"members")) )
        return FALSE;

    // If the class list isn't locked, any element that is a featureMember
    // will do. 
    if( !IsClassListLocked() )
        return TRUE;

    // otherwise, find a class with the desired element name.
    for( int i = 0; i < GetClassCount(); i++ )
    {
        if( EQUAL(pszElement,GetClass(i)->GetElementName()) )
            return TRUE;
    }

    return FALSE;
}

/************************************************************************/
/*                IsCityGMLGenericAttributeElement()                    */
/************************************************************************/

int GMLReader::IsCityGMLGenericAttributeElement( const char *pszElement, void* attr )

{
    if( strcmp(pszElement, "stringAttribute") != 0 &&
        strcmp(pszElement, "intAttribute") != 0 &&
        strcmp(pszElement, "doubleAttribute") != 0 )
        return FALSE;

    if( m_poState->m_poFeature == NULL )
        return FALSE;

    char* pszVal = m_poGMLHandler->GetAttributeValue(attr, "name");
    if (pszVal == NULL)
        return FALSE;

    GMLFeatureClass *poClass = m_poState->m_poFeature->GetClass();

    // If the schema is not yet locked, then any simple element
    // is potentially an attribute.
    if( !poClass->IsSchemaLocked() )
    {
        CPLFree(pszVal);
        return TRUE;
    }

    for( int i = 0; i < poClass->GetPropertyCount(); i++ )
    {
        if( strcmp(poClass->GetProperty(i)->GetSrcElement(),pszVal) == 0 )
        {
            CPLFree(pszVal);
            return TRUE;
        }
    }

    CPLFree(pszVal);
    return FALSE;
}

/************************************************************************/
/*                         IsAttributeElement()                         */
/************************************************************************/

int GMLReader::IsAttributeElement( const char *pszElement )

{
    if( m_poState->m_poFeature == NULL )
        return FALSE;

    GMLFeatureClass *poClass = m_poState->m_poFeature->GetClass();

    // If the schema is not yet locked, then any simple element
    // is potentially an attribute.
    if( !poClass->IsSchemaLocked() )
        return TRUE;

    // Otherwise build the path to this element into a single string
    // and compare against known attributes.
    CPLString osElemPath;

    if( m_poState->m_nPathLength == 0 )
        osElemPath = pszElement;
    else
    {
        osElemPath = m_poState->m_pszPath;
        osElemPath += "|";
        osElemPath += pszElement;
    }

    for( int i = 0; i < poClass->GetPropertyCount(); i++ )
        if( strcmp(poClass->GetProperty(i)->GetSrcElement(),osElemPath) == 0 )
            return TRUE;

    return FALSE;
}

/************************************************************************/
/*                              PopState()                              */
/************************************************************************/

void GMLReader::PopState()

{
    if( m_poState != NULL )
    {
#ifdef HAVE_XERCES
        if( !bUseExpatReader && m_poState->m_poFeature != NULL &&
            m_poCompleteFeature == NULL )
        {
            m_poCompleteFeature = m_poState->m_poFeature;
            m_poState->m_poFeature = NULL;
        }
#endif

#ifdef HAVE_EXPAT
        if ( bUseExpatReader && m_poState->m_poFeature != NULL )
        {
            ppoFeatureTab = (GMLFeature**)
                    CPLRealloc(ppoFeatureTab,
                                sizeof(GMLFeature*) * (nFeatureTabLength + 1));
            ppoFeatureTab[nFeatureTabLength] = m_poState->m_poFeature;
            nFeatureTabLength++;

            m_poState->m_poFeature = NULL;
        }
#endif

        GMLReadState *poParent;

        poParent = m_poState->m_poParentState;
        
        delete m_poState;
        m_poState = poParent;
    }
}

/************************************************************************/
/*                             PushState()                              */
/************************************************************************/

void GMLReader::PushState( GMLReadState *poState )

{
    poState->m_poParentState = m_poState;
    m_poState = poState;
}

/************************************************************************/
/*                              GetClass()                              */
/************************************************************************/

GMLFeatureClass *GMLReader::GetClass( int iClass ) const

{
    if( iClass < 0 || iClass >= m_nClassCount )
        return NULL;
    else
        return m_papoClass[iClass];
}

/************************************************************************/
/*                              GetClass()                              */
/************************************************************************/

GMLFeatureClass *GMLReader::GetClass( const char *pszName ) const

{
    for( int iClass = 0; iClass < m_nClassCount; iClass++ )
    {
        if( EQUAL(GetClass(iClass)->GetName(),pszName) )
            return GetClass(iClass);
    }

    return NULL;
}

/************************************************************************/
/*                              AddClass()                              */
/************************************************************************/

int GMLReader::AddClass( GMLFeatureClass *poNewClass )

{
    CPLAssert( GetClass( poNewClass->GetName() ) == NULL );

    m_nClassCount++;
    m_papoClass = (GMLFeatureClass **) 
        CPLRealloc( m_papoClass, sizeof(void*) * m_nClassCount );
    m_papoClass[m_nClassCount-1] = poNewClass;

    return m_nClassCount-1;
}

/************************************************************************/
/*                            ClearClasses()                            */
/************************************************************************/

void GMLReader::ClearClasses()

{
    for( int i = 0; i < m_nClassCount; i++ )
        delete m_papoClass[i];
    CPLFree( m_papoClass );

    m_nClassCount = 0;
    m_papoClass = NULL;
}

/************************************************************************/
/*                         SetFeatureProperty()                         */
/*                                                                      */
/*      Set the property value on the current feature, adding the       */
/*      property name to the GMLFeatureClass if required.               */
/*      Eventually this function may also "refine" the property         */
/*      type based on what is encountered.                              */
/************************************************************************/

void GMLReader::SetFeatureProperty( const char *pszElement, 
                                    const char *pszValue )

{
    GMLFeature *poFeature = GetState()->m_poFeature;

    CPLAssert( poFeature  != NULL );

/* -------------------------------------------------------------------- */
/*      Does this property exist in the feature class?  If not, add     */
/*      it.                                                             */
/* -------------------------------------------------------------------- */
    GMLFeatureClass *poClass = poFeature->GetClass();
    int      iProperty;

    for( iProperty=0; iProperty < poClass->GetPropertyCount(); iProperty++ )
    {
        if( strcmp(poClass->GetProperty( iProperty )->GetSrcElement(),
                  pszElement ) == 0 )
            break;
    }
    
    if( iProperty == poClass->GetPropertyCount() )
    {
        if( poClass->IsSchemaLocked() )
        {
            CPLDebug("GML","Encountered property missing from class schema.");
            return;
        }

        CPLString osFieldName;
        
        if( strchr(pszElement,'|') == NULL )
            osFieldName = pszElement;
        else
        {
            osFieldName = strrchr(pszElement,'|') + 1;
            if( poClass->GetPropertyIndex(osFieldName) != -1 )
                osFieldName = pszElement;
        }

        // Does this conflict with an existing property name? 
        while( poClass->GetProperty(osFieldName) != NULL )
        {
            osFieldName += "_";
        }

        GMLPropertyDefn *poPDefn = new GMLPropertyDefn(osFieldName,pszElement);

        if( EQUAL(CPLGetConfigOption( "GML_FIELDTYPES", ""), "ALWAYS_STRING") )
            poPDefn->SetType( GMLPT_String );

        if (poClass->AddProperty( poPDefn ) < 0)
        {
            delete poPDefn;
            return;
        }
    }

/* -------------------------------------------------------------------- */
/*      Set the property                                                */
/* -------------------------------------------------------------------- */
    poFeature->SetProperty( iProperty, pszValue );

/* -------------------------------------------------------------------- */
/*      Do we need to update the property type?                         */
/* -------------------------------------------------------------------- */
    if( !poClass->IsSchemaLocked() )
    {
        poClass->GetProperty(iProperty)->AnalysePropertyValue(
                             poFeature->GetProperty(iProperty));
    }
}

/************************************************************************/
/*                            LoadClasses()                             */
/************************************************************************/

int GMLReader::LoadClasses( const char *pszFile )

{
    // Add logic later to determine reasonable default schema file. 
    if( pszFile == NULL )
        return FALSE;

/* -------------------------------------------------------------------- */
/*      Load the raw XML file.                                          */
/* -------------------------------------------------------------------- */
    VSILFILE       *fp;
    int         nLength;
    char        *pszWholeText;

    fp = VSIFOpenL( pszFile, "rb" );

    if( fp == NULL )
    {
        CPLError( CE_Failure, CPLE_OpenFailed, 
                  "Failed to open file %s.", pszFile );
        return FALSE;
    }

    VSIFSeekL( fp, 0, SEEK_END );
    nLength = (int) VSIFTellL( fp );
    VSIFSeekL( fp, 0, SEEK_SET );

    pszWholeText = (char *) VSIMalloc(nLength+1);
    if( pszWholeText == NULL )
    {
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "Failed to allocate %d byte buffer for %s,\n"
                  "is this really a GMLFeatureClassList file?",
                  nLength, pszFile );
        VSIFCloseL( fp );
        return FALSE;
    }
    
    if( VSIFReadL( pszWholeText, nLength, 1, fp ) != 1 )
    {
        VSIFree( pszWholeText );
        VSIFCloseL( fp );
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "Read failed on %s.", pszFile );
        return FALSE;
    }
    pszWholeText[nLength] = '\0';

    VSIFCloseL( fp );

    if( strstr( pszWholeText, "<GMLFeatureClassList>" ) == NULL )
    {
        VSIFree( pszWholeText );
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "File %s does not contain a GMLFeatureClassList tree.",
                  pszFile );
        return FALSE;
    }

/* -------------------------------------------------------------------- */
/*      Convert to XML parse tree.                                      */
/* -------------------------------------------------------------------- */
    CPLXMLNode *psRoot;

    psRoot = CPLParseXMLString( pszWholeText );
    VSIFree( pszWholeText );

    // We assume parser will report errors via CPL.
    if( psRoot == NULL )
        return FALSE;

    if( psRoot->eType != CXT_Element 
        || !EQUAL(psRoot->pszValue,"GMLFeatureClassList") )
    {
        CPLDestroyXMLNode(psRoot);
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "File %s is not a GMLFeatureClassList document.",
                  pszFile );
        return FALSE;
    }

/* -------------------------------------------------------------------- */
/*      Extract feature classes for all definitions found.              */
/* -------------------------------------------------------------------- */
    CPLXMLNode *psThis;

    for( psThis = psRoot->psChild; psThis != NULL; psThis = psThis->psNext )
    {
        if( psThis->eType == CXT_Element 
            && EQUAL(psThis->pszValue,"GMLFeatureClass") )
        {
            GMLFeatureClass   *poClass;

            poClass = new GMLFeatureClass();

            if( !poClass->InitializeFromXML( psThis ) )
            {
                delete poClass;
                CPLDestroyXMLNode( psRoot );
                return FALSE;
            }

            poClass->SetSchemaLocked( TRUE );

            AddClass( poClass );
        }
    }

    CPLDestroyXMLNode( psRoot );
    
    SetClassListLocked( TRUE );

    return TRUE;
}

/************************************************************************/
/*                            SaveClasses()                             */
/************************************************************************/

int GMLReader::SaveClasses( const char *pszFile )

{
    // Add logic later to determine reasonable default schema file. 
    if( pszFile == NULL )
        return FALSE;

/* -------------------------------------------------------------------- */
/*      Create in memory schema tree.                                   */
/* -------------------------------------------------------------------- */
    CPLXMLNode *psRoot;

    psRoot = CPLCreateXMLNode( NULL, CXT_Element, "GMLFeatureClassList" );

    for( int iClass = 0; iClass < GetClassCount(); iClass++ )
    {
        GMLFeatureClass *poClass = GetClass( iClass );
        
        CPLAddXMLChild( psRoot, poClass->SerializeToXML() );
    }

/* -------------------------------------------------------------------- */
/*      Serialize to disk.                                              */
/* -------------------------------------------------------------------- */
    VSILFILE        *fp;
    int         bSuccess = TRUE;
    char        *pszWholeText = CPLSerializeXMLTree( psRoot );
    
    CPLDestroyXMLNode( psRoot );
 
    fp = VSIFOpenL( pszFile, "wb" );
    
    if( fp == NULL )
        bSuccess = FALSE;
    else if( VSIFWriteL( pszWholeText, strlen(pszWholeText), 1, fp ) != 1 )
        bSuccess = FALSE;
    else
        VSIFCloseL( fp );

    CPLFree( pszWholeText );

    return bSuccess;
}

/************************************************************************/
/*                          PrescanForSchema()                          */
/*                                                                      */
/*      For now we use a pretty dumb approach of just doing a normal    */
/*      scan of the whole file, building up the schema information.     */
/*      Eventually we hope to do a more efficient scan when just        */
/*      looking for schema information.                                 */
/************************************************************************/

int GMLReader::PrescanForSchema( int bGetExtents )

{
    GMLFeature  *poFeature;

    if( m_pszFilename == NULL )
        return FALSE;

    SetClassListLocked( FALSE );

    ClearClasses();
    if( !SetupParser() )
        return FALSE;

    m_bCanUseGlobalSRSName = TRUE;

    while( (poFeature = NextFeature()) != NULL )
    {
        GMLFeatureClass *poClass = poFeature->GetClass();

        if( poClass->GetFeatureCount() == -1 )
            poClass->SetFeatureCount( 1 );
        else
            poClass->SetFeatureCount( poClass->GetFeatureCount() + 1 );

#ifdef SUPPORT_GEOMETRY
        if( bGetExtents )
        {
            OGRGeometry *poGeometry = GML_BuildOGRGeometryFromList(
                poFeature->GetGeometryList(), TRUE, m_bInvertAxisOrderIfLatLong, NULL, m_bConsiderEPSGAsURN);

            if( poGeometry != NULL )
            {
                double  dfXMin, dfXMax, dfYMin, dfYMax;
                OGREnvelope sEnvelope;
                OGRwkbGeometryType eGType = (OGRwkbGeometryType) 
                    poClass->GetGeometryType();

                char* pszSRSName = GML_ExtractSrsNameFromGeometry(poFeature->GetGeometryList(),
                                                                  m_bConsiderEPSGAsURN);
                if (pszSRSName != NULL)
                    m_bCanUseGlobalSRSName = FALSE;
                poClass->MergeSRSName(pszSRSName);
                CPLFree(pszSRSName);

                // Merge geometry type into layer.
                if( poClass->GetFeatureCount() == 1 && eGType == wkbUnknown )
                    eGType = wkbNone;

                poClass->SetGeometryType( 
                    (int) OGRMergeGeometryTypes(
                        eGType, poGeometry->getGeometryType() ) );

                // merge extents.
                if (!poGeometry->IsEmpty())
                {
                    poGeometry->getEnvelope( &sEnvelope );
                    if( poClass->GetExtents(&dfXMin, &dfXMax, &dfYMin, &dfYMax) )
                    {
                        dfXMin = MIN(dfXMin,sEnvelope.MinX);
                        dfXMax = MAX(dfXMax,sEnvelope.MaxX);
                        dfYMin = MIN(dfYMin,sEnvelope.MinY);
                        dfYMax = MAX(dfYMax,sEnvelope.MaxY);
                    }
                    else
                    {
                        dfXMin = sEnvelope.MinX;
                        dfXMax = sEnvelope.MaxX;
                        dfYMin = sEnvelope.MinY;
                        dfYMax = sEnvelope.MaxY;
                    }

                    poClass->SetExtents( dfXMin, dfXMax, dfYMin, dfYMax );
                }
                delete poGeometry;

            }
#endif /* def SUPPORT_GEOMETRY */
        }
        
        delete poFeature;
    }

    for( int i = 0; i < m_nClassCount; i++ )
    {
        GMLFeatureClass *poClass = m_papoClass[i];
        const char* pszSRSName = poClass->GetSRSName();

        if (m_bCanUseGlobalSRSName)
            pszSRSName = m_pszGlobalSRSName;
        
        if (m_bInvertAxisOrderIfLatLong && GML_IsSRSLatLongOrder(pszSRSName))
        {
            OGRSpatialReference oSRS;
            if (oSRS.SetFromUserInput(pszSRSName) == OGRERR_NONE)
            {
                OGR_SRSNode *poGEOGCS = oSRS.GetAttrNode( "GEOGCS" );
                if( poGEOGCS != NULL )
                {
                    poGEOGCS->StripNodes( "AXIS" );

                    char* pszWKT = NULL;
                    if (oSRS.exportToWkt(&pszWKT) == OGRERR_NONE)
                        poClass->SetSRSName(pszWKT);
                    CPLFree(pszWKT);

                    /* So when we have computed the extent, we didn't know yet */
                    /* the SRS to use. Now we know it, we have to fix the extent */
                    /* order */
                    if (m_bCanUseGlobalSRSName)
                    {
                        double  dfXMin, dfXMax, dfYMin, dfYMax;
                        if( poClass->GetExtents(&dfXMin, &dfXMax, &dfYMin, &dfYMax) )
                            poClass->SetExtents( dfYMin, dfYMax, dfXMin, dfXMax );
                    }
                }
            }
        }
    }

    CleanupParser();

    return GetClassCount() > 0;
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/

void GMLReader::ResetReading()

{
    CleanupParser();
    SetFilteredClassName(NULL);
}

/************************************************************************/
/*                          SetGlobalSRSName()                          */
/************************************************************************/

void GMLReader::SetGlobalSRSName( const char* pszGlobalSRSName )
{
    if (m_pszGlobalSRSName == NULL && pszGlobalSRSName != NULL)
    {
        if (strncmp(pszGlobalSRSName, "EPSG:", 5) == 0 &&
            m_bConsiderEPSGAsURN)
        {
            m_pszGlobalSRSName = CPLStrdup(CPLSPrintf("urn:ogc:def:crs:EPSG::%s",
                                                      pszGlobalSRSName+5));
        }
        else
        {
            m_pszGlobalSRSName = CPLStrdup(pszGlobalSRSName);
        }
    }
}

/************************************************************************/
/*                       SetFilteredClassName()                         */
/************************************************************************/

int GMLReader::SetFilteredClassName(const char* pszClassName)
{
    CPLFree(m_pszFilteredClassName);
    m_pszFilteredClassName = (pszClassName) ? CPLStrdup(pszClassName) : NULL;
    return TRUE;
}
