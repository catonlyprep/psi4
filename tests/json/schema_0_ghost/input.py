import numpy as np
import psi4
import json

# Generate JSON data
json_data = {
    "schema_name": "QC_JSON",
    "schema_version": 0,
    "molecule": {
        "geometry": [
            0.0,
            0.0,
            -5.0,
            0.0,
            0.0,
            5.0,
        ],
        "symbols": ["He", "He"],
        "real": [True, False]
    },
    "driver": "energy",
    "model": {
        "method": "SCF",
        "basis": "cc-pVDZ"
    },
    "keywords": {
        "scf_type": "df"
    }
}

# Write expected output
expected_return_result = -2.85518836280515
expected_properties = {
    'calcinfo_nbasis': 10,
    'calcinfo_nmo': 10,
    'calcinfo_nalpha': 1,
    'calcinfo_nbeta': 1,
    'calcinfo_natom': 2,
    'scf_one_electron_energy': -3.8820496359492576,
    'scf_two_electron_energy': 1.0268612731441076,
    'nuclear_repulsion_energy': 0.0,
    'scf_total_energy': -2.85518836280515,
    'return_energy': -2.85518836280515
}

psi4.json_wrapper.run_json(json_data)

with open("output.json", "w") as ofile:  #TEST
    json.dump(json_data, ofile, indent=2)  #TEST

psi4.compare_integers(True, json_data["success"], "JSON Success")  #TEST
psi4.compare_values(expected_return_result, json_data["return_result"], 6, "Return Value")  #TEST

for k in expected_properties.keys():
    psi4.compare_values(expected_properties[k], json_data["properties"][k], 6, k.upper())  #TEST
