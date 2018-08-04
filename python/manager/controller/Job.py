import datetime

from flask_restful import Resource, reqparse, fields, marshal_with, abort

from model.FuzzingJob import fuzz_jobs
from model.FuzzingTarget import targets
from model.FuzzingInputs import inputs
from model.job_inputs import job_inputs

from app import app
import logging

db = app.config['db']

logger = logging.getLogger(__name__)


job_fields = {
    'job_id': fields.Integer(),
    'job_type': fields.String(),
    'status': fields.String(),
    'mutator_state': fields.String(),
    'mutator': fields.String(),
    'instrumentation_type': fields.String(),
    'driver': fields.String(),
    'assign_time': fields.DateTime(dt_format='iso8601'),
    'end_time': fields.DateTime(dt_format='iso8601'),
    'input_ids': fields.List(fields.Integer(attribute='input_id'), attribute='inputs'),
    'seed_file': fields.String(),
}

class JobCtrl(Resource):
    def read(self, id):
        """
        Fetch the db entry for a given job id, or error if not found
        :param id: job_id of the job to be fetched
        :return: list containing the dictionary representing the job object, or a dictionary indicating error.
        """
        job = fuzz_jobs.query.filter_by(job_id=id).first()
        if job is None:
            abort(404, err="not found")
        return job, 200

    def readAll(self, target_id):
        """
        Get all jobs associated with the specified target_id, or error if not found
        :param target_id: target_id for which all jobs should be returned
        :return: list containing all jobs for the given target, or a dictionary indicating error.
        """
        target = targets.query.filter_by(target_id=target_id).first()
        if target is None:
            abort(404, err="not found")
        jobs = fuzz_jobs.query.filter_by(target_id=target_id).all()
        #jobs = [{'job': job} for job in jobs]
        return jobs, 200

    def create(self, data):
        """
        Create a new job.
        :param data: dictionary of attributes for the new job object
        :return: newly created job object on 200, error dictionary on 400
        """
        if data.job_type is not None:
            # Default to "fuzz" type
            type = "fuzz"
        else:
            type = data.job_type
        if data.target_id is None or data.target_id == 0:
            abort(400, err="target_id must be supplied and non-zero")
        else:
            # verify the target exists
            target = targets.query.filter_by(target_id=data.target_id).first()
            if target is None:
                abort(400, err="supplied target_id not found")
        if data.input_ids:
            for input_id in data.input_ids:
                if inputs.query.get(input_id) is None:
                    abort(400, err="supplied input_id not found")
        try:
            job = fuzz_jobs(type, data.target_id,
                            mutator=data.mutator,
                            mutator_state=data.mutator_state,
                            instrumentation_type=data.instrumentation_type,
                            driver=data.driver, seed_file=data.seed_file,
                            )
            if data.input_ids:
                job.inputs = [job_inputs(input_id=input_id) for input_id in data.input_ids]
            db.session.add(job)
            db.session.commit()
        except Exception as e:
            logger.exception('failed to add job')
            abort(400, err="invalid request")

        return job, 200


    # TODO: A way to update existing jobs via the REST API?
    def update(self, id, data):
        job = fuzz_jobs.query.get(id)
        if job is None:
            job = fuzz_jobs(None, None, job_id=id)
            db.session.add(job)

        if data.seed_file is not None:
            job.seed_file = data.seed_file
        if data.status is not None:
            job.status = data.status
        db.session.commit()
        return job, 200

    @marshal_with(job_fields)
    def get(self, id=None):
        """
        Request either a single job (by id) or all jobs for a target (by target_id)
        :return: List of jobs that match the query on 200; error dict on 400
        """
        parser = reqparse.RequestParser()
        parser.add_argument("target_id", type=int)

        args = parser.parse_args()
        # The two options are mutually exclusive
        if id is not None and args.target_id is not None:
            abort(400, err='id and target_id are mutually exclusive')
        # But at least one must be supplied
        if id is None and args.target_id is None:
            abort(400, err='either id or target_id must be supplied')
        if id is not None:
            return self.read(id)
        else:
            return self.readAll(args.target_id)

    @marshal_with(job_fields)
    def post(self):
        """
        Create a new job.
        :return: The job created on 200, error on 400
        """
        parser = reqparse.RequestParser()
        parser.add_argument("job_type", type=str)
        parser.add_argument("target_id", type=int)
        parser.add_argument("mutator", type=str)
        parser.add_argument("mutator_state", type=str)
        parser.add_argument("instrumentation_type", type=str)
        parser.add_argument("driver", type=str)
        parser.add_argument("input_ids", type=int, action='append', location='json')
        parser.add_argument("seed_file", type=str)
        args = parser.parse_args()
        return self.create(args)

    @marshal_with(job_fields)
    def put(self, id):
        """
        Update a job.
        """
        parser = reqparse.RequestParser()
        parser.add_argument("seed_file", type=str)
        parser.add_argument("status", type=str)
        args = parser.parse_args()
        return self.update(id, args)