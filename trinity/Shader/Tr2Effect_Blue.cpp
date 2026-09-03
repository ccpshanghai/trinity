// Copyright © 2023 CCP ehf.

#include "StdAfx.h"
#include "Tr2Effect.h"
#include "Include/ITriEffectParameter.h"
#include "Tr2Shader.h"

BLUE_DEFINE( Tr2Effect );

#if BLUE_WITH_PYTHON

// ---------------------------------------------------------------
// PyGetParameterAnnotations
//
// Gets a python dict of the parameter annotations, using a parameter object or string for the name
// ---------------------------------------------------------------
static PyObject* PyGetParameterAnnotations( PyObject* self, PyObject* args )
{
	Tr2Effect* pThis = BluePythonCast<Tr2Effect*>( self );

	PyObject* parameterObject = NULL;
	if( !PyArg_ParseTuple( args, "O", &parameterObject ) )
		return NULL;

	std::string parameterNameString;

	if( !pThis->GetShaderStateInterface() )
	{
		Py_INCREF( Py_None );
		return Py_None;
	}

	// If we got passed a string, look up the string, if we got passed a parameter, look up the name of that
	if( PyVerCompat::IsPyString( parameterObject ) )
	{
		parameterNameString = FromPython<std::string>( parameterObject );
	}
	else
	{
		ITriEffectParameterPtr iParameterObject( parameterObject );
		if( iParameterObject != NULL )
		{
			parameterNameString = iParameterObject->GetParameterName();
		}
		else
		{
			Py_INCREF( Py_None );
			return Py_None;
		}
	}

	PyObject* tmpValue = NULL;
	PyObject* annotationDict = PyDict_New();

	auto annotations = pThis->GetShaderStateInterface()->GetParameterAnnotations( parameterNameString.c_str() );
	if( annotations == nullptr )
	{
		Py_INCREF( Py_None );
		return Py_None;
	}
	for( auto annotation = annotations->begin(); annotation != annotations->end(); ++annotation )
	{
		if( annotation->type == Tr2EffectParameterAnnotation::BOOL )
		{
			tmpValue = ToPython( annotation->boolValue );
			PyDict_SetItemString( annotationDict, annotation->name, tmpValue );
			Py_DECREF( tmpValue );
		}
		else if( annotation->type == Tr2EffectParameterAnnotation::INT )
		{
			tmpValue = ToPython( annotation->intValue );
			PyDict_SetItemString( annotationDict, annotation->name, tmpValue );
			Py_DECREF( tmpValue );
		}
		else if( annotation->type == Tr2EffectParameterAnnotation::FLOAT )
		{
			tmpValue = ToPython( annotation->floatValue );
			PyDict_SetItemString( annotationDict, annotation->name, tmpValue );
			Py_DECREF( tmpValue );
		}
		else if( annotation->type == Tr2EffectParameterAnnotation::STRING )
		{
			tmpValue = ToPython( annotation->stringValue );
			PyDict_SetItemString( annotationDict, annotation->name, tmpValue );
			Py_DECREF( tmpValue );
		}
	}

	return annotationDict;
}
#endif

Be::VarChooser EffectFileChooser[] = {
	{ "SELECT_EFFECT",
	  BeCast( 0 ),
	  "Effect source (.fx)|*.fx|All Files (*.*)|*.*" },
	{ 0 }
};


#if BLUE_WITH_PYTHON
// ---------------------------------------------------------------
// PyGetActualEffectPath
//
// The path the engine will actually open for a given ".fx" res path, or None when the rewrite
// cannot be done (the path is not under /effect/). A module function rather than a method
// because the caller asks it BEFORE building an effect -- "is this staged for this backend?" --
// and there is no Tr2Effect yet to ask.
// ---------------------------------------------------------------
static PyObject* PyGetActualEffectPath( PyObject* self, PyObject* args )
{
	const char* path = NULL;
	if( !PyArg_ParseTuple( args, "s", &path ) )
	{
		return NULL;
	}

	std::string actualPath;
	if( !Tr2Effect::ActualEffectPath( path, actualPath ) )
	{
		Py_RETURN_NONE;
	}
	return PyUnicode_FromStringAndSize( actualPath.c_str(), actualPath.size() );
}
#endif

const Be::ClassInfo* Tr2Effect::ExposeToBlue()
{
	EXPOSURE_BEGIN( Tr2Effect, "" )
		MAP_INTERFACE( Tr2Effect )
		MAP_INTERFACE( Tr2Material )
		MAP_INTERFACE( INotify )
		MAP_INTERFACE( IInitialize )
		MAP_INTERFACE( IListNotify )

		////////////////////////////////////////////////////////////////////////////
		//               name
		MAP_ATTRIBUTE(
			"name",
			m_name,
			"A name to describe the effect. Not used by the engine, an authoring convenience.",
			Be::READWRITE | Be::PERSIST )

		////////////////////////////////////////////////////////////////////////////
		// Effect Resource Blue File
		MAP_ATTRIBUTE_WITH_CHOOSER(
			"effectFilePath",
			m_effectFilePath,
			"The shader file to use",
			Be::READWRITE | Be::PERSIST | Be::NOTIFY,
			EffectFileChooser )

		MAP_ATTRIBUTE(
			"actualEffectFilePath",
			m_actualEffectFilePath,
			"Path to compiled effect, after shader model mangling",
			Be::READ )

		////////////////////////////////////////////////////////////////////////////
		// The currently loaded effect resource
		MAP_ATTRIBUTE(
			"effectResource",
			m_effectResource,
			"na",
			Be::READ )
		MAP_ATTRIBUTE(
			"options",
			m_options,
			"",
			Be::READ | Be::PERSIST )
		////////////////////////////////////////////////////////////////////////////
		//
		MAP_ATTRIBUTE(
			"parameters",
			m_parameters,
			"Effect Parameters",
			Be::READ | Be::PERSIST )
		MAP_ATTRIBUTE(
			"constParameters",
			m_constParameters,
			"Constant Effect Parameters",
			Be::READ | Be::PERSIST )
		////////////////////////////////////////////////////////////////////////////
		//
		MAP_ATTRIBUTE(
			"resources",
			m_resources,
			"Effect Resources",
			Be::READ | Be::PERSIST )
		MAP_ATTRIBUTE(
			"samplerOverrides",
			m_samplerOverrides,
			"Sampler state overrides",
			Be::READ | Be::PERSIST )

		MAP_METHOD(
			"GetParameterAnnotations",
			PyGetParameterAnnotations,
			"Gets the annotations on a parameter\n"
			":param parameter: either parameter name or an effect parameter object\n"
			":type parameter: str | ITriEffectParameter\n"
			":rtype: dict" )
		MAP_METHOD_AND_WRAP( "PopulateParameters", PopulateParameters, "Populates the parameter list with the appropriate parameters" )
		MAP_METHOD_AND_WRAP( "PruneParameters", PruneParameters, "Removes parameters from the parameter list that are not used by the effect" )
		MAP_METHOD_AND_WRAP(
			"IsParameterUsedByTechnique",
			IsParameterUsedByTechnique,
			"Returns True if the parameter name is used by the current technique\n"
			":param name: parameter name" )
		MAP_METHOD_AND_WRAP( "RebuildCachedData", RebuildCachedDataInternal, "Call this after adding/removing parameters/resources" )

		MAP_METHOD_AND_WRAP( "StartUpdate", StartUpdate, "Temporarily block list notifications from causing a data rebuild" )
		MAP_METHOD_AND_WRAP( "EndUpdate", EndUpdate, "Reactivate list notifications, and do a data rebuild" )

	EXPOSURE_END()
}

#if BLUE_WITH_PYTHON
MAP_FUNCTION(
	"GetActualEffectPath",
	PyGetActualEffectPath,
	"The path the engine will actually open for an effect, given its .fx res path.\n"
	"\n"
	"'/effect/' becomes '/effect.<platform>/' and the extension becomes the current shader\n"
	"model's ('.sm_hi', '.sm_lo', '.sm_depth'). Both halves are the engine's -- the platform\n"
	"token is a compile-time constant that differs per Apple SDK, and the shader model is live\n"
	"renderer state -- so a caller wanting to know whether its effect is staged has to ask\n"
	"rather than reproduce the rewrite.\n"
	":param path: the .fx res path\n"
	":type path: str\n"
	":rtype: str | None" );
#endif
