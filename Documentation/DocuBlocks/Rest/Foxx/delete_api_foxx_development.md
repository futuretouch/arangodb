@startDocuBlock delete_api_foxx_development

@RESTHEADER{DELETE /_api/foxx/development, Disable the development mode, disableFoxxDevelopmentMode}

@RESTDESCRIPTION
Puts the service at the given mount path into production mode.

When running ArangoDB in a cluster with multiple Coordinators this will
replace the service on all other Coordinators with the version on this
Coordinator.

@RESTQUERYPARAMETERS

@RESTQUERYPARAM{mount,string,required}
Mount path of the installed service.

@RESTRETURNCODES

@RESTRETURNCODE{200}
Returned if the request was successful.

@endDocuBlock
