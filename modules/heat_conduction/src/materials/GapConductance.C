/****************************************************************/
/* MOOSE - Multiphysics Object Oriented Simulation Environment  */
/*                                                              */
/*          All contents are licensed under LGPL V2.1           */
/*             See LICENSE for full restrictions                */
/****************************************************************/

#include "GapConductance.h"

// MOOSE includes
#include "Function.h"
#include "MooseMesh.h"
#include "MooseVariable.h"
#include "PenetrationLocator.h"
#include "SystemBase.h"
#include "AddVariableAction.h"

// libMesh includes
#include "libmesh/string_to_enum.h"

template <>
InputParameters
validParams<GapConductance>()
{
  InputParameters params = validParams<Material>();
  params += GapConductance::actionParameters();

  params.addRequiredCoupledVar("variable", "Temperature variable");

  // Node based
  params.addCoupledVar("gap_distance", "Distance across the gap");
  params.addCoupledVar("gap_temp", "Temperature on the other side of the gap");
  params.addParam<Real>("gap_conductivity", 1.0, "The thermal conductivity of the gap material");
  params.addParam<FunctionName>(
      "gap_conductivity_function",
      "Thermal conductivity of the gap material as a function.  Multiplied by gap_conductivity.");
  params.addCoupledVar("gap_conductivity_function_variable",
                       "Variable to be used in the gap_conductivity_function in place of time");

  // Quadrature based
  params.addParam<bool>("quadrature",
                        false,
                        "Whether or not to do quadrature point based gap heat "
                        "transfer.  If this is true then gap_distance and "
                        "gap_temp should NOT be provided (and will be "
                        "ignored); however, paired_boundary and variable are "
                        "then required.");
  params.addParam<BoundaryName>("paired_boundary", "The boundary to be penetrated");

  params.addParam<Real>("stefan_boltzmann", 5.669e-8, "The Stefan-Boltzmann constant");

  params.addParam<bool>("use_displaced_mesh",
                        true,
                        "Whether or not this object should use the "
                        "displaced mesh for computation.  Note that in "
                        "the case this is true but no displacements "
                        "are provided in the Mesh block the "
                        "undisplaced mesh will still be used.");

  return params;
}

InputParameters
GapConductance::actionParameters()
{
  InputParameters params = emptyInputParameters();
  params.addParam<std::string>(
      "appended_property_name", "", "Name appended to material properties to make them unique");
  MooseEnum gap_geom_types("PLATE CYLINDER SPHERE");
  params.addParam<MooseEnum>("gap_geometry_type", gap_geom_types, "Gap calculation type.");

  params.addParam<RealVectorValue>("cylinder_axis_point_1",
                                   "Start point for line defining cylindrical axis");
  params.addParam<RealVectorValue>("cylinder_axis_point_2",
                                   "End point for line defining cylindrical axis");
  params.addParam<RealVectorValue>("sphere_origin", "Origin for sphere geometry");

  params.addRangeCheckedParam<Real>("emissivity_1",
                                    0.0,
                                    "emissivity_1>=0 & emissivity_1<=1",
                                    "The emissivity of the fuel surface");
  params.addRangeCheckedParam<Real>("emissivity_2",
                                    0.0,
                                    "emissivity_2>=0 & emissivity_2<=1",
                                    "The emissivity of the cladding surface");

  params.addParam<bool>(
      "warnings", false, "Whether to output warning messages concerning nodes not being found");

  MooseEnum orders(AddVariableAction::getNonlinearVariableOrders());
  params.addParam<MooseEnum>("order", orders, "The finite element order");

  // Common
  params.addRangeCheckedParam<Real>(
      "min_gap", 1e-6, "min_gap>=0", "A minimum gap (denominator) size");
  params.addRangeCheckedParam<Real>(
      "max_gap", 1e6, "max_gap>=0", "A maximum gap (denominator) size");

  return params;
}

GapConductance::GapConductance(const InputParameters & parameters)
  : Material(parameters),
    _appended_property_name(getParam<std::string>("appended_property_name")),
    _temp(coupledValue("variable")),
    _gap_geometry_params_set(false),
    _gap_geometry_type(GapConductance::PLATE),
    _quadrature(getParam<bool>("quadrature")),
    _gap_temp(0),
    _gap_distance(88888),
    _radius(0),
    _r1(0),
    _r2(0),
    _has_info(false),
    _gap_distance_value(_quadrature ? _zero : coupledValue("gap_distance")),
    _gap_temp_value(_quadrature ? _zero : coupledValue("gap_temp")),
    _gap_conductance(declareProperty<Real>("gap_conductance" + _appended_property_name)),
    _gap_conductance_dT(declareProperty<Real>("gap_conductance" + _appended_property_name + "_dT")),
    _gap_thermal_conductivity(declareProperty<Real>("gap_conductivity")),
    _gap_conductivity(getParam<Real>("gap_conductivity")),
    _gap_conductivity_function(isParamValid("gap_conductivity_function")
                                   ? &getFunction("gap_conductivity_function")
                                   : NULL),
    _gap_conductivity_function_variable(isCoupled("gap_conductivity_function_variable")
                                            ? &coupledValue("gap_conductivity_function_variable")
                                            : NULL),
    _stefan_boltzmann(getParam<Real>("stefan_boltzmann")),
    _emissivity(getParam<Real>("emissivity_1") != 0.0 && getParam<Real>("emissivity_2") != 0.0
                    ? 1.0 / getParam<Real>("emissivity_1") + 1.0 / getParam<Real>("emissivity_2") -
                          1
                    : 0.0),
    _min_gap(getParam<Real>("min_gap")),
    _max_gap(getParam<Real>("max_gap")),
    _temp_var(_quadrature ? getVar("variable", 0) : NULL),
    _penetration_locator(NULL),
    _serialized_solution(_quadrature ? &_temp_var->sys().currentSolution() : NULL),
    _dof_map(_quadrature ? &_temp_var->sys().dofMap() : NULL),
    _warnings(getParam<bool>("warnings"))
{
  if (_quadrature)
  {
    if (!parameters.isParamValid("paired_boundary"))
      mooseError(std::string("No 'paired_boundary' provided for ") + _name);
  }
  else
  {
    if (!isCoupled("gap_distance"))
      mooseError(std::string("No 'gap_distance' provided for ") + _name);

    if (!isCoupled("gap_temp"))
      mooseError(std::string("No 'gap_temp' provided for ") + _name);
  }

  if (_quadrature)
  {
    _penetration_locator = &_subproblem.geomSearchData().getQuadraturePenetrationLocator(
        parameters.get<BoundaryName>("paired_boundary"),
        getParam<std::vector<BoundaryName>>("boundary")[0],
        Utility::string_to_enum<Order>(parameters.get<MooseEnum>("order")));
  }
}

void
GapConductance::initialSetup()
{
  setGapGeometryParameters(_pars, _coord_sys, _gap_geometry_type, _p1, _p2);
}

void
GapConductance::setGapGeometryParameters(const InputParameters & params,
                                         const Moose::CoordinateSystemType coord_sys,
                                         GAP_GEOMETRY & gap_geometry_type,
                                         Point & p1,
                                         Point & p2)
{
  if (params.isParamSetByUser("gap_geometry_type"))
  {
    gap_geometry_type =
        GapConductance::GAP_GEOMETRY(int(params.get<MooseEnum>("gap_geometry_type")));
  }
  else
  {
    if (coord_sys == Moose::COORD_XYZ)
      gap_geometry_type = GapConductance::PLATE;
    else if (coord_sys == Moose::COORD_RZ)
      gap_geometry_type = GapConductance::CYLINDER;
    else if (coord_sys == Moose::COORD_RSPHERICAL)
      gap_geometry_type = GapConductance::SPHERE;
  }

  if (gap_geometry_type == GapConductance::PLATE)
  {
    if (coord_sys == Moose::COORD_RSPHERICAL)
      ::mooseError("'gap_geometry_type = PLATE' cannot be used with models having a spherical "
                   "coordinate system.");
  }
  else if (gap_geometry_type == GapConductance::CYLINDER)
  {
    if (coord_sys == Moose::COORD_XYZ)
    {
      if (!params.isParamValid("cylinder_axis_point_1") ||
          !params.isParamValid("cylinder_axis_point_2"))
        ::mooseError("For 'gap_geometry_type = CYLINDER' to be used with a Cartesian model, "
                     "'cylinder_axis_point_1' and 'cylinder_axis_point_2' must be specified.");
      p1 = params.get<RealVectorValue>("cylinder_axis_point_1");
      p2 = params.get<RealVectorValue>("cylinder_axis_point_2");
    }
    else if (coord_sys == Moose::COORD_RZ)
    {
      if (params.isParamValid("cylinder_axis_point_1") ||
          params.isParamValid("cylinder_axis_point_2"))
        ::mooseError("The 'cylinder_axis_point_1' and 'cylinder_axis_point_2' cannot be specified "
                     "with axisymmetric models.  The y-axis is used as the cylindrical axis of "
                     "symmetry.");
      p1 = Point(0, 0, 0);
      p2 = Point(0, 1, 0);
    }
    else if (coord_sys == Moose::COORD_RSPHERICAL)
      ::mooseError("'gap_geometry_type = CYLINDER' cannot be used with models having a spherical "
                   "coordinate system.");
  }
  else if (gap_geometry_type == GapConductance::SPHERE)
  {
    if (coord_sys == Moose::COORD_XYZ || coord_sys == Moose::COORD_RZ)
    {
      if (!params.isParamValid("sphere_origin"))
        ::mooseError("For 'gap_geometry_type = SPHERE' to be used with a Cartesian or axisymmetric "
                     "model, 'sphere_origin' must be specified.");
      p1 = params.get<RealVectorValue>("sphere_origin");
    }
    else if (coord_sys == Moose::COORD_RSPHERICAL)
    {
      if (params.isParamValid("sphere_origin"))
        ::mooseError("The 'sphere_origin' cannot be specified with spherical models.  x=0 is used "
                     "as the spherical origin.");
      p1 = Point(0, 0, 0);
    }
  }
}

void
GapConductance::computeQpProperties()
{
  computeGapValues();
  computeQpConductance();
}

void
GapConductance::computeQpConductance()
{
  if (_has_info)
  {
    _gap_conductance[_qp] = h_conduction() + h_radiation();
    _gap_conductance_dT[_qp] = dh_conduction();
  }
  else
  {
    _gap_conductance[_qp] = 0;
    _gap_conductance_dT[_qp] = 0;
  }
}

Real
GapConductance::h_conduction()
{
  _gap_thermal_conductivity[_qp] = gapK();
  return _gap_thermal_conductivity[_qp] /
         gapLength(_gap_geometry_type, _radius, _r1, _r2, _min_gap, _max_gap);
}

Real
GapConductance::dh_conduction()
{
  return 0.0;
}

Real
GapConductance::h_radiation()
{
  /*
   Gap conductance due to radiation is based on the diffusion approximation:

      qr = sigma*Fe*(Tf^4 - Tc^4) ~ hr(Tf - Tc)
         where sigma is the Stefan-Boltztmann constant, Fe is an emissivity function, Tf and Tc
         are the fuel and clad absolute temperatures, respectively, and hr is the radiant gap
         conductance. Solving for hr,

      hr = sigma*Fe*(Tf^4 - Tc^4) / (Tf - Tc)
         which can be factored to give:

      hr = sigma*Fe*(Tf^2 + Tc^2) * (Tf + Tc)

   Approximating the fuel-clad gap as infinite parallel planes, the emissivity function is given by:

      Fe = 1 / (1/ef + 1/ec - 1)
  */

  if (_emissivity == 0.0)
    return 0.0;

  const Real temp_func =
      (_temp[_qp] * _temp[_qp] + _gap_temp * _gap_temp) * (_temp[_qp] + _gap_temp);
  return _stefan_boltzmann * temp_func / _emissivity;
}

Real
GapConductance::dh_radiation()
{
  if (_emissivity == 0.0)
    return 0.0;

  const Real temp_func = 3 * _temp[_qp] * _temp[_qp] + _gap_temp * (2 * _temp[_qp] + _gap_temp);
  return _stefan_boltzmann * temp_func / _emissivity;
}

Real
GapConductance::gapLength(const GapConductance::GAP_GEOMETRY & gap_geom,
                          Real radius,
                          Real r1,
                          Real r2,
                          Real min_gap,
                          Real max_gap)
{
  if (gap_geom == GapConductance::CYLINDER)
    return gapCyl(radius, r1, r2, min_gap, max_gap);
  else if (gap_geom == GapConductance::SPHERE)
    return gapSphere(radius, r1, r2, min_gap, max_gap);
  else
    return gapRect(r2 - r1, min_gap, max_gap);
}

Real
GapConductance::gapRect(Real distance, Real min_gap, Real max_gap)
{
  return std::max(min_gap, std::min(distance, max_gap));
}

Real
GapConductance::gapCyl(Real radius, Real r1, Real r2, Real min_denom, Real max_denom)
{
  Real denominator = radius * std::log(r2 / r1);
  return std::max(min_denom, std::min(denominator, max_denom));
}

Real
GapConductance::gapSphere(Real radius, Real r1, Real r2, Real min_denom, Real max_denom)
{
  Real denominator = std::pow(radius, 2.0) * ((1.0 / r1) - (1.0 / r2));
  return std::max(min_denom, std::min(denominator, max_denom));
}

Real
GapConductance::gapK()
{
  Real gap_conductivity = _gap_conductivity;

  if (_gap_conductivity_function)
  {
    if (_gap_conductivity_function_variable)
      gap_conductivity *= _gap_conductivity_function->value(
          (*_gap_conductivity_function_variable)[_qp], _q_point[_qp]);
    else
      gap_conductivity *= _gap_conductivity_function->value(_t, _q_point[_qp]);
  }

  return gap_conductivity;
}

void
GapConductance::computeGapValues()
{
  if (!_quadrature)
  {
    _has_info = true;
    _gap_temp = _gap_temp_value[_qp];
    _gap_distance = _gap_distance_value[_qp];
  }
  else
  {
    Node * qnode = _mesh.getQuadratureNode(_current_elem, _current_side, _qp);
    PenetrationInfo * pinfo = _penetration_locator->_penetration_info[qnode->id()];

    _gap_temp = 0.0;
    _gap_distance = 88888;
    _has_info = false;

    if (pinfo)
    {
      _gap_distance = pinfo->_distance;
      _has_info = true;

      Elem * slave_side = pinfo->_side;
      std::vector<std::vector<Real>> & slave_side_phi = pinfo->_side_phi;
      std::vector<dof_id_type> slave_side_dof_indices;

      _dof_map->dof_indices(slave_side, slave_side_dof_indices, _temp_var->number());

      for (unsigned int i = 0; i < slave_side_dof_indices.size(); ++i)
      {
        // The zero index is because we only have one point that the phis are evaluated at
        _gap_temp += slave_side_phi[i][0] * (*(*_serialized_solution))(slave_side_dof_indices[i]);
      }
    }
    else
    {
      if (_warnings)
        mooseWarning("No gap value information found for node ",
                     qnode->id(),
                     " on processor ",
                     processor_id(),
                     " at coordinate ",
                     Point(*qnode));
    }
  }

  Point current_point(_q_point[_qp]);
  computeGapRadii(
      _gap_geometry_type, current_point, _p1, _p2, _gap_distance, _normals[_qp], _r1, _r2, _radius);
}

void
GapConductance::computeGapRadii(const GapConductance::GAP_GEOMETRY gap_geometry_type,
                                const Point & current_point,
                                const Point & p1,
                                const Point & p2,
                                const Real & gap_distance,
                                const Point & current_normal,
                                Real & r1,
                                Real & r2,
                                Real & radius)
{
  if (gap_geometry_type == GapConductance::CYLINDER)
  {
    // The vector _p1 + t*(_p2-_p1) defines the cylindrical axis.  The point along this
    // axis closest to current_point is found by the following for t:
    const Point p2p1(p2 - p1);
    const Point p1pc(p1 - current_point);
    const Real t = -(p1pc * p2p1) / p2p1.norm_sq();

    // The nearest point on the cylindrical axis to current_point is p.
    const Point p(p1 + t * p2p1);
    Point rad_vec(current_point - p);
    Real rad = rad_vec.norm();
    rad_vec /= rad;
    Real rad_dot_norm = rad_vec * current_normal;

    if (rad_dot_norm > 0)
    {
      r1 = rad;
      r2 = rad - gap_distance; // note, gap_distance is negative
      radius = r1;
    }
    else if (rad_dot_norm < 0)
    {
      r1 = rad + gap_distance;
      r2 = rad;
      radius = r2;
    }
    else
      ::mooseError("Issue with cylindrical flux calc. normals.\n");
  }
  else if (gap_geometry_type == GapConductance::SPHERE)
  {
    const Point origin_to_curr_point(current_point - p1);
    const Real normal_dot = origin_to_curr_point * current_normal;
    const Real curr_point_radius = origin_to_curr_point.norm();
    if (normal_dot > 0) // on inside surface
    {
      r1 = curr_point_radius;
      r2 = curr_point_radius - gap_distance; // gap_distance is negative
      radius = r1;
    }
    else if (normal_dot < 0) // on outside surface
    {
      r1 = curr_point_radius + gap_distance; // gap_distance is negative
      r2 = curr_point_radius;
      radius = r2;
    }
    else
      ::mooseError("Issue with spherical flux calc. normals. \n");
  }
  else
  {
    r2 = -gap_distance;
    r1 = 0;
    radius = 0;
  }
}
