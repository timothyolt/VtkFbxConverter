/**
 * \file vtkOsgConverter.cpp
 * 2012-04-20 LB Initial implementation
 *
 * Implementation of vtkFbxConverter class
 */

// ** INCLUDES **
#include "VtkFbxConverter.h"
#include "VtkFbxHelper.h"

#include <vtkActor.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkCompositeDataGeometryFilter.h>
#include <vtkDataArray.h>
#include <vtkDataSet.h>
#include <vtkDataSetMapper.h>
#include <vtkGeometryFilter.h>
#include <vtkImageData.h>
#include <vtkMapper.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkSmartPointer.h>
#include <vtkTexture.h>
#include <vtkUnsignedCharArray.h>
#include <vtkCellDataToPointData.h>
#include <vtkLookupTable.h>
#include <vtkDiscretizableColorTransferFunction.h>
#include <vtkPNGWriter.h>
#include <vtkProperty.h>
#include <vtkLookupTable.h>
#include <vtkPolyDataNormals.h>
#include <vtkTriangleFilter.h>
#include <vtkUnstructuredGrid.h>

#include <vtkDataSetSurfaceFilter.h>

#include <fbxsdk.h>

VtkFbxConverter::VtkFbxConverter(vtkActor* actor, FbxScene* scene)
: _actor(actor), _scene(scene)
{

}

VtkFbxConverter::~VtkFbxConverter()
{
	//delete _node;
	if( remove((_name + std::string("_vtk_texture.png")).c_str()) != 0)
		perror("Error deleting file");
}


FbxNode* VtkFbxConverter::getNode() const
{
	return _node;
}

bool VtkFbxConverter::convert(std::string name)
{
	cout << "VtkFbxConverter::convert() started ..." << endl;
	_name = VtkFbxHelper::extractBaseNameWithoutExtension(name);

	// dont export when not visible
	if (_actor->GetVisibility() == 0)
		return false;

	vtkMapper* actorMapper = _actor->GetMapper();
	// see if the actor has a mapper. it could be an assembly
	if (actorMapper == NULL)
		return NULL;

	vtkDataObject* inputDO = actorMapper->GetInputDataObject(0, 0);
	if (inputDO == NULL)
		return NULL;

	_node = _scene->GetRootNode();

	vtkSmartPointer<vtkPolyData> pd;
	if(inputDO->IsA("vtkCompositeDataSet"))
	{
		cout << "  Converting composite data set to poly data ..." << endl;
		vtkCompositeDataGeometryFilter* gf = vtkCompositeDataGeometryFilter::New();
#ifdef NEW_VTK
		gf->SetInputData(inputDO);
#else
		gf->SetInput(inputDO);
#endif
		gf->Update();
		pd = gf->GetOutput();
		gf->Delete();
	}
	else if (inputDO->GetDataObjectType() != VTK_POLY_DATA)
	{
		cout << "  Converting data set to poly data ..." << endl;
		vtkDataSetSurfaceFilter* gf = vtkDataSetSurfaceFilter::New();
		//gf->MergingOff();
#ifdef NEW_VTK
		gf->SetInputData(inputDO);
#else
		gf->SetInput(inputDO);
#endif
		gf->Update();
		pd = gf->GetOutput();
		gf->Delete();
	}
	else
	{
		cout << "  Loaded data is already poly data." << endl;
		pd = static_cast<vtkPolyData*>(inputDO);
	}

	// poly data should be valid now
	if(pd == NULL)
	{
		cout << "  Aborting: Data set could be converted to polydata!" << endl;
		return false;
	}

	int numPoints = pd->GetNumberOfPoints();
	if(numPoints == 0)
	{
		cout << "  Aborting: No points in the data set!" << endl;
		return false;
	}

	// Check normals
	if(!VtkFbxHelper::GetPointNormals(pd) && !VtkFbxHelper::GetCellNormals(pd))
	{
		// Generate normals
		std::cout << "  Generating normals ..." << std::endl;
		vtkSmartPointer<vtkPolyDataNormals> normalGenerator =
			vtkSmartPointer<vtkPolyDataNormals>::New();
#ifdef NEW_VTK
		normalGenerator->SetInputData(pd);
#else
		normalGenerator->SetInput(pd);
#endif
		normalGenerator->ComputePointNormalsOn();
		normalGenerator->ComputeCellNormalsOff();
		//normalGenerator->FlipNormalsOn();
		normalGenerator->Update();
		pd = normalGenerator->GetOutput();
	}


	std::vector<vtkSmartPointer<vtkPolyData> > subGrids;
	subGrids = VtkFbxHelper::subdivideByMaxPoints(pd, 65000);

	for (std::vector<vtkSmartPointer<vtkPolyData> >::iterator i = subGrids.begin(); i != subGrids.end(); ++i)
	{
		vtkPolyData* polydata = *i;
		cout << "    Points: " << (polydata)->GetNumberOfPoints() << endl;
		cout << "    Cells: " << (polydata)->GetNumberOfCells() << endl;

		FbxMesh* mesh = FbxMesh::Create(_scene, _name.c_str());

		// -- Vertices --
		vtkIdType numVertices = polydata->GetNumberOfPoints(); // pd->GetNumberOfVerts(); ?
		cout << "    NumVertices: " << numVertices << std::endl;
		if (numVertices == 0)
			continue;
		mesh->InitControlPoints(numVertices);
		FbxVector4* controlPoints = mesh->GetControlPoints();
		for (int i = 0; i < numVertices; i++)
		{
			double* aVertex = polydata->GetPoint(i);
			controlPoints[i] = FbxVector4(-aVertex[0], aVertex[2], aVertex[1]);
		}

		// Compute bounding box and translate all points so
		// that new bounding box equals (0, 0, 0)
		mesh->ComputeBBox();
		FbxDouble3 bbmin = mesh->BBoxMin;
		FbxDouble3 bbmax = mesh->BBoxMax;
		FbxDouble3 boundingBoxCenter((bbmax[0] + bbmin[0]) / 2,
									 (bbmax[1] + bbmin[1]) / 2,
									 (bbmax[2] + bbmin[2]) / 2);
		cout << "    Object Center: " << boundingBoxCenter[0] << ", " << boundingBoxCenter[1] << ", " << boundingBoxCenter[2] << endl;
		for (int i = 0; i < numVertices; i++)
			controlPoints[i] = controlPoints[i] - boundingBoxCenter;


		// Get Layer 0.
		FbxLayer* layer = mesh->GetLayer(0);
		if (layer == NULL)
		{
			mesh->CreateLayer();
			layer = mesh->GetLayer(0);
		}

		// -- Normals --
		vtkDataArray* vtkNormals = NULL;
		// TODO: normals on cell data: pd->GetCellData()->GetNormals();
		vtkPointData* pntData = polydata->GetPointData();
		vtkNormals = pntData->GetNormals();
		if (vtkNormals)
		{
			// We want to have one normal for each vertex (or control point),
			// so we set the mapping mode to eByControlPoint.
			FbxLayerElementNormal* layerElementNormal= FbxLayerElementNormal::Create(mesh, "");

			layerElementNormal->SetMappingMode(FbxLayerElement::eByControlPoint);
			vtkIdType numNormals = vtkNormals->GetNumberOfTuples();
			cout << "    NumNormals: " << numNormals << std::endl;
			for (int i = 0; i < numNormals; i++)
			{
				double* aNormal = vtkNormals->GetTuple(i);
				layerElementNormal->GetDirectArray().Add(FbxVector4(-aNormal[0], aNormal[2], aNormal[1]));
			}

			layer->SetNormals(layerElementNormal);
		}

		// -- Texture coordinates --
		vtkDataArray* vtkTexCoords = pntData->GetTCoords();
		if (vtkTexCoords != NULL)
		{
			// Create UV for Diffuse channel.
			FbxLayerElementUV* lUVDiffuseLayer = FbxLayerElementUV::Create(mesh, "DiffuseUV");
			lUVDiffuseLayer->SetMappingMode(FbxLayerElement::eByControlPoint);
			lUVDiffuseLayer->SetReferenceMode(FbxLayerElement::eIndexToDirect);
			layer->SetUVs(lUVDiffuseLayer, FbxLayerElement::eTextureDiffuse);

			vtkIdType numCoords = vtkTexCoords->GetNumberOfTuples();
			cout << "    NumTexCoords: " << numCoords << std::endl;
			for (int i = 0; i < numCoords; i++)
			{
				double texCoords[3];
				vtkTexCoords->GetTuple(i, texCoords);
				lUVDiffuseLayer->GetDirectArray().Add(FbxVector2(texCoords[0], texCoords[1])); // TODO: ordering?
			}

			//Now we have set the UVs as eIndexToDirect reference and in eByPolygonVertex  mapping mode
			//we must update the size of the index array.
			lUVDiffuseLayer->GetIndexArray().SetCount(numVertices);
			for (int i = 0; i < numVertices; i++)
				lUVDiffuseLayer->GetIndexArray().SetAt(i, i);
		}

		// -- Vertex Colors --
		vtkUnsignedCharArray* vtkColors = this->getColors(polydata);
		vtkIdType numColors = 0;
		if (vtkColors)
			numColors = vtkColors->GetNumberOfTuples();

		if (numColors > 0)
		{
			FbxGeometryElementVertexColor* vertexColorElement = mesh->CreateElementVertexColor();
			int scalarMode = _actor->GetMapper()->GetScalarMode();
			if (scalarMode == VTK_SCALAR_MODE_USE_POINT_DATA ||
				scalarMode == VTK_SCALAR_MODE_USE_POINT_FIELD_DATA)
			{
				cout << "    Colors on points." << endl;
				vertexColorElement->SetMappingMode(FbxGeometryElement::eByControlPoint);
			}
			else if(scalarMode == VTK_SCALAR_MODE_USE_CELL_DATA ||
					scalarMode == VTK_SCALAR_MODE_USE_CELL_FIELD_DATA)
			{
				cout << "    Colors on cells." << endl;
				vertexColorElement->SetMappingMode(FbxGeometryElement::eByPolygon);
			}
			else
			{
				if(numColors == numVertices)
					vertexColorElement->SetMappingMode(FbxGeometryElement::eByControlPoint);
				else if(numColors == polydata->GetNumberOfCells())
					vertexColorElement->SetMappingMode(FbxGeometryElement::eByPolygon);
				else
				{
					cout << "    Aborting: Do not know how to process colors!" << endl;
					return false;
				}
			}
			vertexColorElement->SetReferenceMode(FbxGeometryElement::eDirect);

			unsigned char aColor[4];
			for (int i = 0; i < numColors; i++)
			{
				vtkColors->GetTupleValue(i, aColor);
				float r = ((float) aColor[0]) / 255.0f;
				float g = ((float) aColor[1]) / 255.0f;
				float b = ((float) aColor[2]) / 255.0f;
				vertexColorElement->GetDirectArray().Add(FbxColor(r, g, b, 1.0));
			}
		}

		cout << "    NumColors: " << numColors << endl;

		// -- Polygons --
		vtkSmartPointer<vtkCellArray> pCells = polydata->GetPolys();
		if(pCells->GetNumberOfCells() == 0)
		{
			cout << "    Converting triangle strips to normal triangles ..." << endl;
			vtkSmartPointer<vtkTriangleFilter> triangleFilter = vtkSmartPointer<vtkTriangleFilter>::New();
#ifdef NEW_VTK
			triangleFilter->SetInputData(polydata);
#else
			triangleFilter->SetInput(polydata);
#endif
			triangleFilter->Update();
			pCells = triangleFilter->GetOutput()->GetPolys();
		}
		cout << "    NumPolyCells: " << pCells->GetNumberOfCells() << std::endl;
		createMeshStructure(pCells, mesh, true); // Ordering has to be flipped


		pCells = pd->GetVerts();
		cout << "    NumPointCells: " << pCells->GetNumberOfCells() << std::endl;
		createMeshStructure(pCells, mesh);

		FbxLayerElementMaterial* layerElementMaterial = mesh->CreateElementMaterial();
		layerElementMaterial->SetMappingMode(FbxGeometryElement::eAllSame);
		layerElementMaterial->SetReferenceMode(FbxGeometryElement::eIndexToDirect);

		// -- Node --
		FbxNode* subnode = FbxNode::Create(_scene, _name.c_str());
		subnode->SetNodeAttribute(mesh);

		// Translate the object back to its originally calculated bounding box centre
		// This and the vertex translation aligns the bounding box centre and the
		// pivot point in Unity
		subnode->LclTranslation.Set(boundingBoxCenter);

		// -- Material --
		subnode->AddMaterial(this->getMaterial(_actor->GetProperty(), _actor->GetTexture(),
											 (bool)actorMapper->GetScalarVisibility(), _scene));

		_node->AddChild(subnode);

		cout << endl;
	}
	// -- Meta data --
	//createUserProperties(_node);

	cout << "VtkFbxConverter::convert() finished" << endl;

	return true;
}

FbxTexture* VtkFbxConverter::getTexture(vtkTexture* texture, FbxScene* scene)
{
	// -- Texture --
	if (!texture)
		return NULL;

	std::string textureName = _name + std::string("_vtk_texture.png");
	vtkPNGWriter* pngWriter = vtkPNGWriter::New();
#if NEW_VTK
	pngWriter->SetInputData(texture->GetInput());
#else
	pngWriter->SetInput(texture->GetInput());
#endif
	pngWriter->SetFileName(textureName.c_str());
	pngWriter->Write();

	FbxFileTexture* fbxTexture = FbxFileTexture::Create(scene, "DiffuseTexture");
	fbxTexture->SetTextureUse(FbxTexture::eStandard);
	fbxTexture->SetMappingType(FbxTexture::eUV);
	fbxTexture->SetMaterialUse(FbxFileTexture::eModelMaterial);
	//fbxTexture->SetAlphaSource (FbxTexture::eBlack);
	fbxTexture->SetFileName(textureName.c_str());

	int* size = texture->GetInput()->GetDimensions();
	std::cout << "    Texture size: " << size[0] << " x " << size[1] << std::endl;

	return fbxTexture;
}

FbxSurfacePhong* VtkFbxConverter::getMaterial(vtkProperty* prop, vtkTexture* texture,
	bool scalarVisibility, FbxScene* scene)
{
	if (!prop)
		return NULL;

	double white[] = {1.0, 1.0, 1.0, 1.0};
	double* diffuseColor = white;
	if(!scalarVisibility)
		diffuseColor = prop->GetDiffuseColor();
	double* ambientColor = prop->GetAmbientColor();
	double* specularColor = prop->GetSpecularColor();
	double specularPower = prop->GetSpecularPower();
	double diffuse = prop->GetDiffuse();
	double ambient = prop->GetAmbient();
	double specular = prop->GetSpecular();
	double opacity = prop->GetOpacity();

	FbxSurfacePhong* material = FbxSurfacePhong::Create(scene,
		(_name + std::string("_material")).c_str());

	// Generate primary and secondary colors.
	material->Emissive.Set(FbxDouble3(0.0, 0.0, 0.0));
	material->Ambient.Set(FbxDouble3(ambientColor[0],
		ambientColor[1], ambientColor[2]));
	material->AmbientFactor.Set(ambient);

	// Add texture for diffuse channel
	FbxTexture* fbxTexture = VtkFbxConverter::getTexture(texture, scene);
	if (fbxTexture)
	{
		material->Diffuse.ConnectSrcObject(fbxTexture);
		cout << "    Connecting texture ..." << endl;
	}
	else
	{
		material->Diffuse.Set(FbxDouble4(diffuseColor[0],
			diffuseColor[1], diffuseColor[2], 1.0 - opacity));
		material->DiffuseFactor.Set(diffuse);
	}

	material->TransparencyFactor.Set(opacity);
	material->ShadingModel.Set("Phong");
	material->Shininess.Set(specularPower);
	material->Specular.Set(FbxDouble3(specularColor[0],
		specularColor[1], specularColor[2]));
	material->SpecularFactor.Set(specular);

	return material;
}

vtkUnsignedCharArray* VtkFbxConverter::getColors(vtkPolyData* pd) const
{
	vtkMapper* actorMapper = _actor->GetMapper();
	// Get the color range from actors lookup table
	double range[2] = {0, 0};
	vtkLookupTable* actorLut = static_cast<vtkLookupTable*>(actorMapper->GetLookupTable());
	if(actorLut)
	{
		cout << "    Getting color range from lut ..." << endl; // Without this cout it crashes??!
		actorLut->GetTableRange(range);
	}

	// Copy mapper to a new one
	vtkPolyDataMapper* pm = vtkPolyDataMapper::New();
	// Convert cell data to point data
	if(false && (
	   actorMapper->GetScalarMode() == VTK_SCALAR_MODE_USE_CELL_DATA ||
	   actorMapper->GetScalarMode() == VTK_SCALAR_MODE_USE_CELL_FIELD_DATA))
	{
		cout << "    Converting cell to point data ..." << endl;
		vtkCellDataToPointData* cellDataToPointData = vtkCellDataToPointData::New();
		cellDataToPointData->PassCellDataOff();
#ifdef NEW_VTK
		cellDataToPointData->SetInputData(pd);
#else
		cellDataToPointData->SetInput(pd);
#endif
		cellDataToPointData->Update();
		pd = cellDataToPointData->GetPolyDataOutput();
		cellDataToPointData->Delete();

		pm->SetScalarMode(VTK_SCALAR_MODE_USE_POINT_DATA);
	}
	else
		pm->SetScalarMode(actorMapper->GetScalarMode());

#ifdef NEW_VTK
	pm->SetInputData(pd);
#else
	pm->SetInput(pd);
#endif
	pm->SetScalarVisibility(actorMapper->GetScalarVisibility());

	vtkLookupTable* lut = NULL;
	// ParaView Exporter
	if (dynamic_cast<vtkDiscretizableColorTransferFunction*>(actorMapper->GetLookupTable()))
		lut = actorLut;
	// Clone the lut in OGS because otherwise the original lut gets destroyed
	else
	{
		lut = vtkLookupTable::New();
		lut->DeepCopy(actorLut);
		lut->Build();
	}
	pm->SetLookupTable(lut);
	pm->SetScalarRange(range);
	pm->Update();

	if(pm->GetScalarMode() == VTK_SCALAR_MODE_USE_POINT_FIELD_DATA ||
	   pm->GetScalarMode() == VTK_SCALAR_MODE_USE_CELL_FIELD_DATA )
	{
		if(actorMapper->GetArrayAccessMode() == VTK_GET_ARRAY_BY_ID )
			pm->ColorByArrayComponent(actorMapper->GetArrayId(),
									  actorMapper->GetArrayComponent());
		else
			pm->ColorByArrayComponent(actorMapper->GetArrayName(),
									  actorMapper->GetArrayComponent());
	}

	return pm->MapScalars(1.0);
}

unsigned int VtkFbxConverter::createMeshStructure(vtkSmartPointer<vtkCellArray> cells, FbxMesh* mesh, const bool flipOrdering) const
{
	unsigned int numPrimitives = 0;

	if (cells->GetNumberOfCells() > 0)
	{
		vtkIdType npts, * pts;
		for (cells->InitTraversal(); cells->GetNextCell(npts, pts); numPrimitives++)
		{
			mesh->BeginPolygon(-1, -1, -1, false);
			if(flipOrdering)
			{
				for (int i = 0; i < npts; i++)
					mesh->AddPolygon(pts[i]);
			}
			else
			{
				// Flip polygon winding.
				for (int i = npts; i > 0; i--)
					mesh->AddPolygon(pts[i-1]);
			}
			mesh->EndPolygon();
		}
	}

	return numPrimitives;
}

void VtkFbxConverter::addUserProperty(const std::string name, const bool value)
{
	FbxProperty property = FbxProperty::Create(_node->GetChild(0), FbxBoolDT, name.c_str(), "");
	property.ModifyFlag(FbxPropertyAttr::eUserDefined, true);
	property.Set(value);
}

void VtkFbxConverter::addUserProperty(const std::string name, const float value)
{
	FbxProperty property = FbxProperty::Create(_node->GetChild(0), FbxFloatDT, name.c_str(), "");
	property.ModifyFlag(FbxPropertyAttr::eUserDefined, true);
	property.Set(value);
}

void VtkFbxConverter::addUserProperty(const std::string name, const int value)
{
	FbxProperty property = FbxProperty::Create(_node->GetChild(0), FbxIntDT, name.c_str(), "");
	property.ModifyFlag(FbxPropertyAttr::eUserDefined, true);
	property.Set(value);
}

void VtkFbxConverter::addUserProperty(const std::string name, const std::string value)
{
	FbxProperty property = FbxProperty::Create(_node->GetChild(0), FbxStringDT, name.c_str(), "");
	property.ModifyFlag(FbxPropertyAttr::eUserDefined, true);
	property.Set(value);
}

void VtkFbxConverter::addUserProperty(const std::string name, FbxColor value)
{
	FbxProperty property = FbxProperty::Create(_node->GetChild(0), FbxColor3DT, name.c_str(), "");
	property.ModifyFlag(FbxPropertyAttr::eUserDefined, true);
	property.Set(value);
}

// void VtkFbxConverter::addUserProperty(const std::string name, FbxDouble4 value)
// {
	// FbxProperty property = FbxProperty::Create(_node->GetChild(0), FbxDouble4DT, name.c_str(), "");
	// property.ModifyFlag(FbxPropertyAttr::eUserDefined, true);
	// property.Set(value);
// }
