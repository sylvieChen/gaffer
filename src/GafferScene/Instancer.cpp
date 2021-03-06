//////////////////////////////////////////////////////////////////////////
//
//  Copyright (c) 2012, John Haddon. All rights reserved.
//  Copyright (c) 2013, Image Engine Design Inc. All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions are
//  met:
//
//      * Redistributions of source code must retain the above
//        copyright notice, this list of conditions and the following
//        disclaimer.
//
//      * Redistributions in binary form must reproduce the above
//        copyright notice, this list of conditions and the following
//        disclaimer in the documentation and/or other materials provided with
//        the distribution.
//
//      * Neither the name of John Haddon nor the names of
//        any other contributors to this software may be used to endorse or
//        promote products derived from this software without specific prior
//        written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
//  IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
//  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
//  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
//  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
//  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
//  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
//  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
//  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//////////////////////////////////////////////////////////////////////////

#include "GafferScene/Instancer.h"

#include "Gaffer/Context.h"
#include "Gaffer/StringPlug.h"

#include "IECoreScene/Primitive.h"

#include "IECore/DataAlgo.h"
#include "IECore/MessageHandler.h"
#include "IECore/NullObject.h"
#include "IECore/VectorTypedData.h"

#include "boost/lexical_cast.hpp"

#include "tbb/blocked_range.h"
#include "tbb/parallel_reduce.h"

#include <functional>
#include <unordered_map>

using namespace std;
using namespace std::placeholders;
using namespace tbb;
using namespace Imath;
using namespace IECore;
using namespace IECoreScene;
using namespace Gaffer;
using namespace GafferScene;

//////////////////////////////////////////////////////////////////////////
// EngineData
//////////////////////////////////////////////////////////////////////////

// Custom Data derived class used to encapsulate the data and
// logic needed to generate instances. We are deliberately omitting
// a custom TypeId etc because this is just a private class.
class Instancer::EngineData : public Data
{

	public :

		EngineData(
			ConstObjectPtr object,
			const std::string &index,
			const std::string &id,
			const std::string &position,
			const std::string &orientation,
			const std::string &scale,
			const std::string &attributes
		)
			:	m_indices( nullptr ),
				m_ids( nullptr ),
				m_positions( nullptr ),
				m_orientations( nullptr ),
				m_scales( nullptr ),
				m_uniformScales( nullptr )
		{
			m_primitive = runTimeCast<const Primitive>( object );
			if( !m_primitive )
			{
				return;
			}

			if( const IntVectorData *indices = m_primitive->variableData<IntVectorData>( index ) )
			{
				m_indices = &indices->readable();
				if( m_indices->size() != numPoints() )
				{
					throw IECore::Exception( "Index primitive variable has incorrect size" );
				}
			}

			if( const IntVectorData *ids = m_primitive->variableData<IntVectorData>( id ) )
			{
				m_ids = &ids->readable();
				if( m_ids->size() != numPoints() )
				{
					throw IECore::Exception( "Id primitive variable has incorrect size" );
				}
			}

			if( const V3fVectorData *p = m_primitive->variableData<V3fVectorData>( position ) )
			{
				m_positions = &p->readable();
				if( m_positions->size() != numPoints() )
				{
					throw IECore::Exception( "Position primitive variable has incorrect size" );
				}
			}

			if( const QuatfVectorData *o = m_primitive->variableData<QuatfVectorData>( orientation ) )
			{
				m_orientations = &o->readable();
				if( m_orientations->size() != numPoints() )
				{
					throw IECore::Exception( "Orientation primitive variable has incorrect size" );
				}
			}

			if( const V3fVectorData *s = m_primitive->variableData<V3fVectorData>( scale ) )
			{
				m_scales = &s->readable();
				if( m_scales->size() != numPoints() )
				{
					throw IECore::Exception( "Scale primitive variable has incorrect size" );
				}
			}
			else if( const FloatVectorData *s = m_primitive->variableData<FloatVectorData>( scale ) )
			{
				m_uniformScales = &s->readable();
				if( m_uniformScales->size() != numPoints() )
				{
					throw IECore::Exception( "Scale primitive variable has incorrect size" );
				}
			}

			if( m_ids )
			{
				for( size_t i = 0; i<numPoints(); ++i )
				{
					m_idsToPointIndices[(*m_ids)[i]] = i;
				}
			}

			initAttributes( attributes );
		}

		size_t numPoints() const
		{
			return m_primitive ? m_primitive->variableSize( PrimitiveVariable::Vertex ) : 0;
		}

		size_t instanceId( size_t pointIndex ) const
		{
			return m_ids ? (*m_ids)[pointIndex] : pointIndex;
		}

		size_t pointIndex( const InternedString &name ) const
		{
			const size_t i = boost::lexical_cast<size_t>( name );
			if( !m_ids )
			{
				return i;
			}

			IdsToPointIndices::const_iterator it = m_idsToPointIndices.find( i );
			if( it == m_idsToPointIndices.end() )
			{
				throw IECore::Exception( "Invalid id" );
			}

			return it->second;
		}

		size_t instanceIndex( size_t pointIndex ) const
		{
			return m_indices ? (*m_indices)[pointIndex] : 0;
		}

		M44f instanceTransform( size_t pointIndex ) const
		{
			M44f result;
			if( m_positions )
			{
				result.translate( (*m_positions)[pointIndex] );
			}
			if( m_orientations )
			{
				result = (*m_orientations)[pointIndex].toMatrix44() * result;
			}
			if( m_scales )
			{
				result.scale( (*m_scales)[pointIndex] );
			}
			if( m_uniformScales )
			{
				result.scale( V3f( (*m_uniformScales)[pointIndex] ) );
			}
			return result;
		}

		size_t numInstanceAttributes() const
		{
			return m_attributeCreators.size();
		}

		void instanceAttributesHash( size_t pointIndex, MurmurHash &h ) const
		{
			h.append( m_attributesHash );
			h.append( (uint64_t)pointIndex );
		}

		CompoundObjectPtr instanceAttributes( size_t pointIndex ) const
		{
			CompoundObjectPtr result = new CompoundObject;
			CompoundObject::ObjectMap &writableResult = result->members();
			for( const auto &attributeCreator : m_attributeCreators )
			{
				writableResult[attributeCreator.first] = attributeCreator.second( pointIndex );
			}
			return result;
		}

	protected :

		void copyFrom( const Object *other, CopyContext *context ) override
		{
			Data::copyFrom( other, context );
			msg( Msg::Warning, "EngineData::copyFrom", "Not implemented" );
		}

		void save( SaveContext *context ) const override
		{
			Data::save( context );
			msg( Msg::Warning, "EngineData::save", "Not implemented" );
		}

		void load( LoadContextPtr context ) override
		{
			Data::load( context );
			msg( Msg::Warning, "EngineData::load", "Not implemented" );
		}

	private :

		typedef std::function<DataPtr ( size_t )> AttributeCreator;

		struct MakeAttributeCreator
		{

			template<typename T>
			AttributeCreator operator()( const TypedData<vector<T>> *data )
			{
				return std::bind( &createAttribute<T>, data->readable(), ::_1 );
			}

			template<typename T>
			AttributeCreator operator()( const GeometricTypedData<vector<T>> *data )
			{
				return std::bind( &createGeometricAttribute<T>, data->readable(), data->getInterpretation(), ::_1 );
			}

			AttributeCreator operator()( const Data *data )
			{
				throw IECore::InvalidArgumentException( "Expected VectorTypedData" );
			}

			private :

				template<typename T>
				static DataPtr createAttribute( const vector<T> &values, size_t index )
				{
					return new TypedData<T>( values[index] );
				}

				template<typename T>
				static DataPtr createGeometricAttribute( const vector<T> &values, GeometricData::Interpretation interpretation, size_t index )
				{
					return new GeometricTypedData<T>( values[index], interpretation );
				}

		};

		void initAttributes( const std::string &attributes )
		{
			for( auto &primVar : m_primitive->variables )
			{
				if( primVar.second.interpolation != PrimitiveVariable::Vertex )
				{
					continue;
				}
				if( !StringAlgo::matchMultiple( primVar.first, attributes ) )
				{
					continue;
				}
				DataPtr d = primVar.second.expandedData();
				AttributeCreator attributeCreator = dispatch( d.get(), MakeAttributeCreator() );
				m_attributeCreators[primVar.first] = attributeCreator;
				m_attributesHash.append( primVar.first );
				d->hash( m_attributesHash );
			}
		}

		IECoreScene::ConstPrimitivePtr m_primitive;
		const std::vector<int> *m_indices;
		const std::vector<int> *m_ids;
		const std::vector<Imath::V3f> *m_positions;
		const std::vector<Imath::Quatf> *m_orientations;
		const std::vector<Imath::V3f> *m_scales;
		const std::vector<float> *m_uniformScales;

		typedef std::unordered_map <int, size_t> IdsToPointIndices;
		IdsToPointIndices m_idsToPointIndices;

		boost::container::flat_map<InternedString, AttributeCreator> m_attributeCreators;
		MurmurHash m_attributesHash;

};

//////////////////////////////////////////////////////////////////////////
// Instancer
//////////////////////////////////////////////////////////////////////////

IE_CORE_DEFINERUNTIMETYPED( Instancer );

size_t Instancer::g_firstPlugIndex = 0;

static const IECore::InternedString idContextName( "instancer:id" );

Instancer::Instancer( const std::string &name )
	:	BranchCreator( name )
{
	storeIndexOfNextChild( g_firstPlugIndex );
	addChild( new StringPlug( "name", Plug::In, "instances" ) );
	addChild( new ScenePlug( "instances" ) );
	addChild( new StringPlug( "index", Plug::In, "instanceIndex" ) );
	addChild( new StringPlug( "id", Plug::In, "instanceId" ) );
	addChild( new StringPlug( "position", Plug::In, "P" ) );
	addChild( new StringPlug( "orientation", Plug::In ) );
	addChild( new StringPlug( "scale", Plug::In ) );
	addChild( new StringPlug( "attributes", Plug::In ) );
	addChild( new ObjectPlug( "__engine", Plug::Out, NullObject::defaultNullObject() ) );
	addChild( new AtomicCompoundDataPlug( "__instanceChildNames", Plug::Out, new CompoundData ) );
}

Instancer::~Instancer()
{
}

Gaffer::StringPlug *Instancer::namePlug()
{
	return getChild<StringPlug>( g_firstPlugIndex );
}

const Gaffer::StringPlug *Instancer::namePlug() const
{
	return getChild<StringPlug>( g_firstPlugIndex );
}

ScenePlug *Instancer::instancesPlug()
{
	return getChild<ScenePlug>( g_firstPlugIndex + 1 );
}

const ScenePlug *Instancer::instancesPlug() const
{
	return getChild<ScenePlug>( g_firstPlugIndex + 1 );
}

Gaffer::StringPlug *Instancer::indexPlug()
{
	return getChild<StringPlug>( g_firstPlugIndex + 2 );
}

const Gaffer::StringPlug *Instancer::indexPlug() const
{
	return getChild<StringPlug>( g_firstPlugIndex + 2 );
}

Gaffer::StringPlug *Instancer::idPlug()
{
	return getChild<StringPlug>( g_firstPlugIndex + 3 );
}

const Gaffer::StringPlug *Instancer::idPlug() const
{
	return getChild<StringPlug>( g_firstPlugIndex + 3 );
}

Gaffer::StringPlug *Instancer::positionPlug()
{
	return getChild<StringPlug>( g_firstPlugIndex + 4 );
}

const Gaffer::StringPlug *Instancer::positionPlug() const
{
	return getChild<StringPlug>( g_firstPlugIndex + 4 );
}

Gaffer::StringPlug *Instancer::orientationPlug()
{
	return getChild<StringPlug>( g_firstPlugIndex + 5 );
}

const Gaffer::StringPlug *Instancer::orientationPlug() const
{
	return getChild<StringPlug>( g_firstPlugIndex + 5 );
}

Gaffer::StringPlug *Instancer::scalePlug()
{
	return getChild<StringPlug>( g_firstPlugIndex + 6 );
}

const Gaffer::StringPlug *Instancer::scalePlug() const
{
	return getChild<StringPlug>( g_firstPlugIndex + 6 );
}

Gaffer::StringPlug *Instancer::attributesPlug()
{
	return getChild<StringPlug>( g_firstPlugIndex + 7 );
}

const Gaffer::StringPlug *Instancer::attributesPlug() const
{
	return getChild<StringPlug>( g_firstPlugIndex + 7 );
}

Gaffer::ObjectPlug *Instancer::enginePlug()
{
	return getChild<ObjectPlug>( g_firstPlugIndex + 8 );
}

const Gaffer::ObjectPlug *Instancer::enginePlug() const
{
	return getChild<ObjectPlug>( g_firstPlugIndex + 8 );
}

Gaffer::AtomicCompoundDataPlug *Instancer::instanceChildNamesPlug()
{
	return getChild<AtomicCompoundDataPlug>( g_firstPlugIndex + 9 );
}

const Gaffer::AtomicCompoundDataPlug *Instancer::instanceChildNamesPlug() const
{
	return getChild<AtomicCompoundDataPlug>( g_firstPlugIndex + 9 );
}

void Instancer::affects( const Plug *input, AffectedPlugsContainer &outputs ) const
{
	BranchCreator::affects( input, outputs );

	if(
		input == inPlug()->objectPlug() ||
		input == indexPlug() ||
		input == idPlug() ||
		input == positionPlug() ||
		input == orientationPlug() ||
		input == scalePlug() ||
		input == attributesPlug()
	)
	{
		outputs.push_back( enginePlug() );
	}

	if(
		input == enginePlug() ||
		input == instancesPlug()->childNamesPlug()
	)
	{
		outputs.push_back( instanceChildNamesPlug() );
	}

	if(
		input == namePlug() ||
		input == instanceChildNamesPlug() ||
		input == instancesPlug()->childNamesPlug()
	)
	{
		outputs.push_back( outPlug()->childNamesPlug() );
	}

	if(
		input == enginePlug() ||
		input == namePlug() ||
		input == instancesPlug()->boundPlug() ||
		input == instancesPlug()->transformPlug() ||
		input == instanceChildNamesPlug()
	)
	{
		outputs.push_back( outPlug()->boundPlug() );
	}

	if(
		input == enginePlug() ||
		input == instancesPlug()->transformPlug()
	)
	{
		outputs.push_back( outPlug()->transformPlug() );
	}

	if( input == instancesPlug()->objectPlug() )
	{
		outputs.push_back( outPlug()->objectPlug() );
	}

	if(
		input == instancesPlug()->attributesPlug() ||
		input == enginePlug()
	)
	{
		outputs.push_back( outPlug()->attributesPlug() );
	}
}

void Instancer::hash( const Gaffer::ValuePlug *output, const Gaffer::Context *context, IECore::MurmurHash &h ) const
{
	BranchCreator::hash( output, context, h );

	if( output == enginePlug() )
	{
		inPlug()->objectPlug()->hash( h );
		indexPlug()->hash( h );
		idPlug()->hash( h );
		positionPlug()->hash( h );
		orientationPlug()->hash( h );
		scalePlug()->hash( h );
		attributesPlug()->hash( h );
	}
	else if( output == instanceChildNamesPlug() )
	{
		enginePlug()->hash( h );
		h.append( instancesPlug()->childNamesHash( ScenePath() ) );
	}
}

void Instancer::compute( Gaffer::ValuePlug *output, const Gaffer::Context *context ) const
{
	// Both the enginePlug and instanceChildNamesPlug are evaluated
	// in a context in which scene:path holds the parent path for a
	// branch.
	if( output == enginePlug() )
	{
		static_cast<ObjectPlug *>( output )->setValue(
			new EngineData(
				inPlug()->objectPlug()->getValue(),
				indexPlug()->getValue(),
				idPlug()->getValue(),
				positionPlug()->getValue(),
				orientationPlug()->getValue(),
				scalePlug()->getValue(),
				attributesPlug()->getValue()
			)
		);
		return;
	}
	else if( output == instanceChildNamesPlug() )
	{
		// Here we compute and cache the child names for all of
		// the /instances/<instanceName> locations at once. We
		// could instead compute them one at a time in
		// computeBranchChildNames() but that would require N
		// passes over the input points, where N is the number
		// of instances.
		ConstEngineDataPtr engine = boost::static_pointer_cast<const EngineData>( enginePlug()->getValue() );
		ConstInternedStringVectorDataPtr instanceNames = instancesPlug()->childNames( ScenePath() );
		vector<vector<InternedString> *> indexedInstanceChildNames;

		CompoundDataPtr result = new CompoundData;
		for( const auto &instanceName : instanceNames->readable() )
		{
			InternedStringVectorDataPtr instanceChildNames = new InternedStringVectorData;
			result->writable()[instanceName] = instanceChildNames;
			indexedInstanceChildNames.push_back( &instanceChildNames->writable() );
		}

		if( indexedInstanceChildNames.size() )
		{
			for( size_t i = 0, e = engine->numPoints(); i < e; ++i )
			{
				size_t instanceIndex = engine->instanceIndex( i ) % indexedInstanceChildNames.size();
				indexedInstanceChildNames[instanceIndex]->push_back( InternedString( engine->instanceId( i ) ) );
			}
		}

		static_cast<AtomicCompoundDataPlug *>( output )->setValue( result );
		return;
	}

	BranchCreator::compute( output, context );
}

void Instancer::hashBranchBound( const ScenePath &parentPath, const ScenePath &branchPath, const Gaffer::Context *context, IECore::MurmurHash &h ) const
{
	if( branchPath.size() < 2 )
	{
		// "/" or "/instances"
		ScenePath path = parentPath;
		path.insert( path.end(), branchPath.begin(), branchPath.end() );
		if( branchPath.size() == 0 )
		{
			path.push_back( namePlug()->getValue() );
		}
		h = hashOfTransformedChildBounds( path, outPlug() );
	}
	else if( branchPath.size() == 2 )
	{
		// "/instances/<instanceName>"
		BranchCreator::hashBranchBound( parentPath, branchPath, context, h );

		engineHash( parentPath, context, h );
		instanceChildNamesHash( parentPath, context, h );
		h.append( branchPath.back() );

		{
			InstanceScope scope( context, branchPath );
			instancesPlug()->transformPlug()->hash( h );
			instancesPlug()->boundPlug()->hash( h );
		}
	}
	else
	{
		// "/instances/<instanceName>/<id>/..."
		InstanceScope instanceScope( context, branchPath );
		h = instancesPlug()->boundPlug()->hash();
	}
}

Imath::Box3f Instancer::computeBranchBound( const ScenePath &parentPath, const ScenePath &branchPath, const Gaffer::Context *context ) const
{
	if( branchPath.size() < 2 )
	{
		// "/" or "/instances"
		ScenePath path = parentPath;
		path.insert( path.end(), branchPath.begin(), branchPath.end() );
		if( branchPath.size() == 0 )
		{
			path.push_back( namePlug()->getValue() );
		}
		return unionOfTransformedChildBounds( path, outPlug() );
	}
	else if( branchPath.size() == 2 )
	{
		// "/instances/<instanceName>"
		//
		// We need to return the union of all the transformed children, but
		// because we have direct access to the engine, we can implement this
		// more efficiently than `unionOfTransformedChildBounds()`.

		ConstEngineDataPtr e = engine( parentPath, context );
		ConstCompoundDataPtr ic = instanceChildNames( parentPath, context );
		const vector<InternedString> &childNames = ic->member<InternedStringVectorData>( branchPath.back() )->readable();

		M44f childTransform;
		Box3f childBound;
		{
			InstanceScope scope( context, branchPath );
			childTransform = instancesPlug()->transformPlug()->getValue();
			childBound = instancesPlug()->boundPlug()->getValue();
		}

		typedef vector<InternedString>::const_iterator Iterator;
		typedef blocked_range<Iterator> Range;

		task_group_context taskGroupContext( task_group_context::isolated );
		return parallel_reduce(
			Range( childNames.begin(), childNames.end() ),
			Box3f(),
			[ &e, &childBound, &childTransform ] ( const Range &r, Box3f u ) {
				for( Iterator i = r.begin(); i != r.end(); ++i )
				{
					const size_t pointIndex = e->pointIndex( *i );
					const M44f m = childTransform * e->instanceTransform( pointIndex );
					const Box3f b = transform( childBound, m );
					u.extendBy( b );
				}
				return u;
			},
			// Union
			[] ( const Box3f &b0, const Box3f &b1 ) {
				Box3f u( b0 );
				u.extendBy( b1 );
				return u;
			},
			tbb::auto_partitioner(),
			// Prevents outer tasks silently cancelling our tasks
			taskGroupContext
		);
	}
	else
	{
		// "/instances/<instanceName>/<id>/..."
		InstanceScope instanceScope( context, branchPath );
		return instancesPlug()->boundPlug()->getValue();
	}
}

void Instancer::hashBranchTransform( const ScenePath &parentPath, const ScenePath &branchPath, const Gaffer::Context *context, IECore::MurmurHash &h ) const
{
	if( branchPath.size() <= 2 )
	{
		// "/" or "/instances" or "/instances/<instanceName>"
		BranchCreator::hashBranchTransform( parentPath, branchPath, context, h );
	}
	else if( branchPath.size() == 3 )
	{
		// "/instances/<instanceName>/<id>"
		BranchCreator::hashBranchTransform( parentPath, branchPath, context, h );
		{
			InstanceScope instanceScope( context, branchPath );
			instancesPlug()->transformPlug()->hash( h );
		}
		engineHash( parentPath, context, h );
		h.append( branchPath[2] );
	}
	else
	{
		// "/instances/<instanceName>/<id>/..."
		InstanceScope instanceScope( context, branchPath );
		h = instancesPlug()->transformPlug()->hash();
	}
}

Imath::M44f Instancer::computeBranchTransform( const ScenePath &parentPath, const ScenePath &branchPath, const Gaffer::Context *context ) const
{
	if( branchPath.size() <= 2 )
	{
		// "/" or "/instances" or "/instances/<instanceName>"
		return M44f();
	}
	else if( branchPath.size() == 3 )
	{
		// "/instances/<instanceName>/<id>"
		M44f result;
		{
			InstanceScope instanceScope( context, branchPath );
			result = instancesPlug()->transformPlug()->getValue();
		}
		ConstEngineDataPtr e = engine( parentPath, context );
		const size_t pointIndex = e->pointIndex( branchPath[2] );
		result = result * e->instanceTransform( pointIndex );
		return result;
	}
	else
	{
		// "/instances/<instanceName>/<id>/..."
		InstanceScope instanceScope( context, branchPath );
		return instancesPlug()->transformPlug()->getValue();
	}
}

void Instancer::hashBranchAttributes( const ScenePath &parentPath, const ScenePath &branchPath, const Gaffer::Context *context, IECore::MurmurHash &h ) const
{
	if( branchPath.size() <= 2 )
	{
		// "/" or "/instances" or "/instances/<instanceName>"
		h = outPlug()->attributesPlug()->defaultValue()->Object::hash();
	}
	else if( branchPath.size() == 3 )
	{
		// "/instances/<instanceName>/<id>"
		BranchCreator::hashBranchAttributes( parentPath, branchPath, context, h );
		{
			{
				InstanceScope instanceScope( context, branchPath );
				instancesPlug()->attributesPlug()->hash( h );
			}
			ConstEngineDataPtr e = engine( parentPath, context );
			if( e->numInstanceAttributes() )
			{
				e->instanceAttributesHash( e->pointIndex( branchPath[2] ), h );
			}
		}
	}
	else
	{
		// "/instances/<instanceName>/<id>/...
		InstanceScope instanceScope( context, branchPath );
		h = instancesPlug()->attributesPlug()->hash();
	}
}

IECore::ConstCompoundObjectPtr Instancer::computeBranchAttributes( const ScenePath &parentPath, const ScenePath &branchPath, const Gaffer::Context *context ) const
{
	if( branchPath.size() <= 2 )
	{
		// "/" or "/instances" or "/instances/<instanceName>"
		return outPlug()->attributesPlug()->defaultValue();
	}
	else if( branchPath.size() == 3 )
	{
		// "/instances/<instanceName>/<id>"
		ConstCompoundObjectPtr baseAttributes;
		{
			InstanceScope instanceScope( context, branchPath );
			baseAttributes = instancesPlug()->attributesPlug()->getValue();
		}

		ConstEngineDataPtr e = engine( parentPath, context );
		if( e->numInstanceAttributes() )
		{
			CompoundObjectPtr attributes = e->instanceAttributes( e->pointIndex( branchPath[2] ) );
			CompoundObject::ObjectMap &writableAttributes = attributes->members();
			for( auto &attribute : baseAttributes->members() )
			{
				writableAttributes.insert( attribute );
			}
			return attributes;
		}
		else
		{
			return baseAttributes;
		}
	}
	else
	{
		// "/instances/<instanceName>/<id>/...
		InstanceScope instanceScope( context, branchPath );
		return instancesPlug()->attributesPlug()->getValue();
	}
}

void Instancer::hashBranchObject( const ScenePath &parentPath, const ScenePath &branchPath, const Gaffer::Context *context, IECore::MurmurHash &h ) const
{
	if( branchPath.size() <= 2 )
	{
		// "/" or "/instances" or "/instances/<instanceName>"
		h = outPlug()->objectPlug()->defaultValue()->Object::hash();
	}
	else
	{
		// "/instances/<instanceName>/<id>/...
		InstanceScope instanceScope( context, branchPath );
		h = instancesPlug()->objectPlug()->hash();
	}
}

IECore::ConstObjectPtr Instancer::computeBranchObject( const ScenePath &parentPath, const ScenePath &branchPath, const Gaffer::Context *context ) const
{
	if( branchPath.size() <= 2 )
	{
		// "/" or "/instances" or "/instances/<instanceName>"
		return outPlug()->objectPlug()->defaultValue();
	}
	else
	{
		// "/instances/<instanceName>/<id>/...
		InstanceScope instanceScope( context, branchPath );
		return instancesPlug()->objectPlug()->getValue();
	}
}

void Instancer::hashBranchChildNames( const ScenePath &parentPath, const ScenePath &branchPath, const Gaffer::Context *context, IECore::MurmurHash &h ) const
{
	if( branchPath.size() == 0 )
	{
		// "/"
		BranchCreator::hashBranchChildNames( parentPath, branchPath, context, h );
		namePlug()->hash( h );
	}
	else if( branchPath.size() == 1 )
	{
		// "/instances"
		h = instancesPlug()->childNamesHash( ScenePath() );
	}
	else if( branchPath.size() == 2 )
	{
		// "/instances/<instanceName>"
		BranchCreator::hashBranchChildNames( parentPath, branchPath, context, h );
		instanceChildNamesHash( parentPath, context, h );
		h.append( branchPath.back() );
	}
	else
	{
		// "/instances/<instanceName>/<id>/..."
		InstanceScope instanceScope( context, branchPath );
		h = instancesPlug()->childNamesPlug()->hash();
	}
}

IECore::ConstInternedStringVectorDataPtr Instancer::computeBranchChildNames( const ScenePath &parentPath, const ScenePath &branchPath, const Gaffer::Context *context ) const
{
	if( branchPath.size() == 0 )
	{
		// "/"
		std::string name = namePlug()->getValue();
		if( name.empty() )
		{
			return outPlug()->childNamesPlug()->defaultValue();
		}
		InternedStringVectorDataPtr result = new InternedStringVectorData();
		result->writable().push_back( name );
		return result;
	}
	else if( branchPath.size() == 1 )
	{
		// "/instances"
		return instancesPlug()->childNames( ScenePath() );
	}
	else if( branchPath.size() == 2 )
	{
		// "/instances/<instanceName>"
		IECore::ConstCompoundDataPtr ic = instanceChildNames( parentPath, context );
		return ic->member<InternedStringVectorData>( branchPath.back() );
	}
	else
	{
		// "/instances/<instanceName>/<id>/..."
		InstanceScope instanceScope( context, branchPath );
		return instancesPlug()->childNamesPlug()->getValue();
	}
}

void Instancer::hashBranchSetNames( const ScenePath &parentPath, const Gaffer::Context *context, IECore::MurmurHash &h ) const
{
	h = instancesPlug()->setNamesPlug()->hash();
}

IECore::ConstInternedStringVectorDataPtr Instancer::computeBranchSetNames( const ScenePath &parentPath, const Gaffer::Context *context ) const
{
	return instancesPlug()->setNamesPlug()->getValue();
}

void Instancer::hashBranchSet( const ScenePath &parentPath, const IECore::InternedString &setName, const Gaffer::Context *context, IECore::MurmurHash &h ) const
{
	BranchCreator::hashBranchSet( parentPath, setName, context, h );

	h.append( instancesPlug()->childNamesHash( ScenePath() ) );
	instanceChildNamesHash( parentPath, context, h );
	instancesPlug()->setPlug()->hash( h );
	namePlug()->hash( h );
}

IECore::ConstPathMatcherDataPtr Instancer::computeBranchSet( const ScenePath &parentPath, const IECore::InternedString &setName, const Gaffer::Context *context ) const
{
	ConstInternedStringVectorDataPtr instanceNames = instancesPlug()->childNames( ScenePath() );
	IECore::ConstCompoundDataPtr instanceChildNames = this->instanceChildNames( parentPath, context );
	ConstPathMatcherDataPtr inputSet = instancesPlug()->setPlug()->getValue();

	PathMatcherDataPtr outputSetData = new PathMatcherData;
	PathMatcher &outputSet = outputSetData->writable();

	vector<InternedString> branchPath( { namePlug()->getValue() } );
	vector<InternedString> instancePath( 1 );

	for( const auto &instanceName : instanceNames->readable() )
	{
		branchPath.resize( 2 );
		branchPath.back() = instanceName;
		instancePath.back() = instanceName;

		PathMatcher instanceSet = inputSet->readable().subTree( instancePath );

		const vector<InternedString> &childNames = instanceChildNames->member<InternedStringVectorData>( instanceName )->readable();

		branchPath.push_back( InternedString() );
		for( const auto &instanceChildName : childNames )
		{
			branchPath.back() = instanceChildName;
			outputSet.addPaths( instanceSet, branchPath );
		}
	}

	return outputSetData;
}

Instancer::ConstEngineDataPtr Instancer::engine( const ScenePath &parentPath, const Gaffer::Context *context ) const
{
	ScenePlug::PathScope scope( context, parentPath );
	return boost::static_pointer_cast<const EngineData>( enginePlug()->getValue() );
}

void Instancer::engineHash( const ScenePath &parentPath, const Gaffer::Context *context, IECore::MurmurHash &h ) const
{
	ScenePlug::PathScope scope( context, parentPath );
	enginePlug()->hash( h );
}

IECore::ConstCompoundDataPtr Instancer::instanceChildNames( const ScenePath &parentPath, const Gaffer::Context *context ) const
{
	ScenePlug::PathScope scope( context, parentPath );
	return instanceChildNamesPlug()->getValue();
}

void Instancer::instanceChildNamesHash( const ScenePath &parentPath, const Gaffer::Context *context, IECore::MurmurHash &h ) const
{
	ScenePlug::PathScope scope( context, parentPath );
	instanceChildNamesPlug()->hash( h );
}

Instancer::InstanceScope::InstanceScope( const Gaffer::Context *context, const ScenePath &branchPath )
	:	EditableScope( context )
{
	assert( branchPath.size() >= 2 );
	ScenePath instancePath;
	instancePath.reserve( 1 + ( branchPath.size() > 3 ? branchPath.size() - 3 : 0 ) );
	instancePath.push_back( branchPath[1] );
	if( branchPath.size() > 3 )
	{
		instancePath.insert( instancePath.end(), branchPath.begin() + 3, branchPath.end() );
	}
	set( ScenePlug::scenePathContextName, instancePath );
}
