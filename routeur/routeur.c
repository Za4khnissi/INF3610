using System;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using Microsoft.Extensions.Configuration;
using Microsoft.Extensions.Logging;
using Server.Interfaces.Services;

namespace Server.Services
{
    public class AzureDevOpsService : IAzureDevOpsService
    {
        private readonly ILogger<AzureDevOpsService> _logger;
        private readonly IConfiguration _configuration;
        private readonly HttpClient _httpClient;
        private readonly string _baseUrl;

        public AzureDevOpsService(
            ILogger<AzureDevOpsService> logger,
            IConfiguration configuration,
            IHttpClientFactory httpClientFactory)
        {
            _logger = logger;
            _configuration = configuration;
            _httpClient = httpClientFactory.CreateClient("AzureDevOps");
            
            string organization = _configuration["AzureDevOps:Organization"];
            string project = _configuration["AzureDevOps:Project"];
            _baseUrl = $"https://dev.azure.com/{organization}/{project}/_apis";
            
            string pat = _configuration["AzureDevOps:PAT"];
            if (!string.IsNullOrEmpty(pat))
            {
                string credentials = Convert.ToBase64String(Encoding.ASCII.GetBytes($":{pat}"));
                _httpClient.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Basic", credentials);
            }
            
            _httpClient.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
        }

        public async Task<string> CreateAndRunPipelineAsync(
            string organization,
            string project,
            string repositoryUrl,
            string repositoryName)
        {
            try
            {
                _logger.LogInformation("Creating Azure Pipeline for repository {RepositoryUrl}", repositoryUrl);
                
                string defaultBranch = await GetRepositoryDefaultBranchAsync(repositoryUrl);
                _logger.LogInformation("Default branch for repository {RepositoryName} is {DefaultBranch}", 
                    repositoryName, defaultBranch);
                
                string pipelineId = await CreatePipelineAsync(organization, project, repositoryUrl, repositoryName, defaultBranch);
                if (pipelineId == null)
                {
                    return null;
                }
                
                var pipelineUrl = await RunPipelineAsync(pipelineId, defaultBranch);
                return pipelineUrl;
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Error creating/running Azure Pipeline for repository {RepositoryUrl}", repositoryUrl);
                return null;
            }
        }
        private async Task<string> CreatePipelineAsync(
            string organization, 
            string project, 
            string repositoryUrl, 
            string repositoryName,
            string defaultBranch)
        {
            try
            {
                string connectionId = _configuration["AzureDevOps:ConnectionId"];

                var urlParts = repositoryUrl.TrimEnd('/').Split('/');
                string owner = urlParts[urlParts.Length - 2];
                string repo = urlParts[urlParts.Length - 1];

                string repoName = $"{owner}/{repo}";
                string repositoryId = $"{owner}/{repo}";
                string pipelineName = $"{repo}";
                
                var buildDefinition = new
                {
                    name = pipelineName,
                    path = $"\\{owner}",
                    type = "build",
                    quality = "definition",
                    repository = new
                    {
                        id = repositoryId,
                        name = repositoryName,
                        repository = repositoryId,
                        url = repositoryUrl,
                        type = "GitHubEnterprise",
                        defaultBranch = defaultBranch,
                        properties = new
                        {
                            connectedServiceId = connectionId
                        }
                    },

                    process = new
                    {
                        type = 2,
                        yamlFilename = "azure-pipelines.yml"
                    },
                    queue = new
                    {
                        name = "Default"
                    }
                };

                string requestUrl = $"{_baseUrl}/build/definitions?api-version=6.0";
                
                string jsonContent = JsonSerializer.Serialize(buildDefinition);
                _logger.LogDebug("Build definition: {Json}", jsonContent);
                
                var content = new StringContent(jsonContent, Encoding.UTF8, "application/json");
                
                var response = await _httpClient.PostAsync(requestUrl, content);
                
                string responseContent = await response.Content.ReadAsStringAsync();
                _logger.LogDebug("Pipeline creation response: {Response}", responseContent);
                
                if (!response.IsSuccessStatusCode)
                {
                    _logger.LogError("Failed to create pipeline. Status: {Status}. Response: {Response}", 
                        response.StatusCode, responseContent);
                    return null;
                }
                
                using (JsonDocument doc = JsonDocument.Parse(responseContent))
                {
                    JsonElement root = doc.RootElement;
                    if (root.TryGetProperty("id", out JsonElement idElement))
                    {
                        return idElement.GetInt32().ToString();
                    }
                }
                
                _logger.LogError("Could not find pipeline ID in response");
                return null;
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Error creating pipeline definition");
                return null;
            }
        }

        private async Task<string> RunPipelineAsync(string pipelineId, string branchName)
        {
            int maxRetries = 3;
            int currentRetry = 0;
            int retryDelayMs = 3000;
            
            while (currentRetry < maxRetries)
            {
                try
                {
                    string requestUrl = $"{_baseUrl}/build/builds?api-version=6.0";
                    
                    var buildRequest = new
                    {
                        definition = new
                        {
                            id = int.Parse(pipelineId)
                        },
                        sourceBranch = $"{branchName}"
                    };
                    
                    string jsonContent = JsonSerializer.Serialize(buildRequest);
                    var content = new StringContent(jsonContent, Encoding.UTF8, "application/json");
                    
                    var response = await _httpClient.PostAsync(requestUrl, content);
                    string responseContent = await response.Content.ReadAsStringAsync();

                    _logger.LogDebug("Build run response: {Response}", responseContent);
                    
                    if (!response.IsSuccessStatusCode)
                    {
                        if (responseContent.Contains("Unable to resolve the reference") && 
                            currentRetry < maxRetries - 1)
                        {
                            currentRetry++;
                            _logger.LogWarning("Branch reference not found (attempt {Attempt}/{MaxAttempts}). Waiting for repository initialization...", 
                                currentRetry, maxRetries);
                            await Task.Delay(retryDelayMs);
                            retryDelayMs *= 2;
                            continue;
                        }
                        
                        _logger.LogError("Failed to run build. Status: {Status}. Response: {Response}", 
                            response.StatusCode, responseContent);
                        
                        if (responseContent.Contains("Unable to resolve the reference") || 
                            responseContent.Contains("Could not find"))
                        {
                            _logger.LogWarning("Repository might still be initializing. YAML file or branch not found.");
                        }
                        
                        return null;
                    }
                    
                    using (JsonDocument doc = JsonDocument.Parse(responseContent))
                    {
                        JsonElement root = doc.RootElement;
                        if (root.TryGetProperty("_links", out JsonElement linksElement) &&
                            linksElement.TryGetProperty("web", out JsonElement webElement) &&
                            webElement.TryGetProperty("href", out JsonElement hrefElement))
                        {
                            return hrefElement.GetString();
                        }
                    }
                    
                    return $"https://dev.azure.com/{_configuration["AzureDevOps:Organization"]}/{_configuration["AzureDevOps:Project"]}/_build/results?buildId={pipelineId}";
                }
                catch (Exception ex)
                {
                    _logger.LogError(ex, "Error running pipeline {PipelineId} (attempt {Attempt}/{MaxAttempts})", 
                        pipelineId, currentRetry + 1, maxRetries);
                    
                    if (currentRetry < maxRetries - 1)
                    {
                        currentRetry++;
                        await Task.Delay(retryDelayMs);
                        retryDelayMs *= 2;
                        continue;
                    }
                    
                    return null;
                }
            }
            
            return null;
        }

        private async Task<string> GetRepositoryDefaultBranchAsync(string repositoryUrl)
        {
            try
            {
                await Task.Delay(5000);

                var urlParts = repositoryUrl.TrimEnd('/').Split('/');
                string owner = urlParts[urlParts.Length - 2];
                string repo = urlParts[urlParts.Length - 1];
                
                string token = _configuration["GitHub:Token"];
                
                using var httpClient = new HttpClient();
                httpClient.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
                httpClient.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", token);
                httpClient.DefaultRequestHeaders.Add("User-Agent", "DevOpsSelfServePortal");
                
                string apiUrl = $"https://githubext.cae.com/api/v3/repos/{owner}/{repo}";
                var response = await httpClient.GetAsync(apiUrl);
                
                if (response.IsSuccessStatusCode)
                {
                    string responseContent = await response.Content.ReadAsStringAsync();
                    using JsonDocument doc = JsonDocument.Parse(responseContent);
                    if (doc.RootElement.TryGetProperty("default_branch", out JsonElement defaultBranchElem))
                    {
                        return defaultBranchElem.GetString();
                    }
                }
                
                return "main";
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "Error getting default branch from repository, using 'main' as fallback");
                return "main";
            }
        }
    }
}

--------------

using Octokit;
using Microsoft.Extensions.Logging;
using Microsoft.AspNetCore.Http;
using System.Collections.Concurrent;
using Server.Interfaces.Services;
using Server.Interfaces.Contexts;
using System.Text;
using Server.Services;

namespace Server.Services
{
    public class GitHubEnterpriseService : IGitHubEnterpriseService
    {
        private readonly ILogger<GitHubEnterpriseService> _logger;
        private readonly IHttpContextAccessor _httpContextAccessor;
        private readonly ILdapService _ldapService;
        private readonly IConfiguration _configuration;
        private readonly IGitHubEnterpriseServerContext _serverContext;
        private readonly ConcurrentDictionary<string, Credentials> _credentialsCache = new();
        private readonly IAzureDevOpsService _azureDevOpsService;

        public GitHubEnterpriseService(
            ILogger<GitHubEnterpriseService> logger,
            IHttpContextAccessor httpContextAccessor,
            ILdapService ldapService,
            IConfiguration configuration,
            IGitHubEnterpriseServerContext serverContext,
            IAzureDevOpsService azureDevOpsService)
        {
            _logger = logger;
            _httpContextAccessor = httpContextAccessor;
            _ldapService = ldapService;
            _configuration = configuration;
            _serverContext = serverContext;
            _azureDevOpsService = azureDevOpsService;
        }

        public async Task<GitHubClient> AuthenticateAsync(string username, string password, int serverId)
        {
            if (!_ldapService.Authenticate(username, password))
            {
                throw new UnauthorizedAccessException("Invalid credentials");
            }

            var credentials = new Credentials(username, password);
            var client = CreateGitHubClient(serverId, credentials);

            try
            {
                await client.User.Current();
                
                _credentialsCache[username] = credentials;
                return client;
            }
            catch (AuthorizationException ex)
            {
                _logger.LogError(ex, "GitHub authentication failed for user {Username} on server {ServerId}", 
                    username, serverId);
                throw new UnauthorizedAccessException("GitHub authentication failed");
            }
        }

        private GitHubClient CreateGitHubClient(int serverId, Credentials credentials = null)
        {
            var server = _serverContext.GetServerById(serverId);
                
            if (server == null || !server.IsActive)
            {
                throw new ArgumentException($"Unknown or inactive GitHub server ID: {serverId}");
            }
            
            string apiUrl = server.ApiUrl ?? $"{server.BaseUrl.TrimEnd('/')}/api/v3";
            
            var client = new GitHubClient(new ProductHeaderValue("DevOpsSelfServePortal"), new Uri(apiUrl));
            
            if (credentials != null)
            {
                client.Credentials = credentials;
            }
            else if (_configuration["GitHub:Token"] is string token && !string.IsNullOrEmpty(token))
            {
                client.Credentials = new Credentials(token);
            }
            
            return client;
        }

        private GitHubClient GetGitHubClient(int serverId)
        {
            string configToken = _configuration["GitHub:Token"];
            if (!string.IsNullOrEmpty(configToken))
            {
                _logger.LogInformation("Using GitHub PAT from configuration for server {ServerId}", serverId);
                return CreateGitHubClient(serverId, new Credentials(configToken));
            }

            var httpContext = _httpContextAccessor.HttpContext;
            
            string username = httpContext?.Session?.GetString("GitHubUsername") ?? string.Empty;
            string password = httpContext?.Session?.GetString("GitHubPassword") ?? string.Empty;
            
            if (!string.IsNullOrEmpty(username) && !string.IsNullOrEmpty(password))
            {
                _logger.LogDebug("Using session credentials for {Username} on server {ServerId}", username, serverId);
                return CreateGitHubClient(serverId, new Credentials(username, password));
            }
            
            string tokenValue = string.Empty;
            if (httpContext?.Request?.Headers.TryGetValue("X-GitHub-Token", out var tokenHeaderValues) == true)
            {
                tokenValue = tokenHeaderValues.ToString() ?? string.Empty;
                if (!string.IsNullOrEmpty(tokenValue))
                {
                    _logger.LogDebug("Using GitHub token from header for server {ServerId}", serverId);
                    return CreateGitHubClient(serverId, new Credentials(tokenValue));
                }
            }
            
            Credentials cachedCredentials = null;
            if (httpContext?.User?.Identity?.Name is string identityUsername && 
                _credentialsCache.TryGetValue(identityUsername, out cachedCredentials))
            {
                _logger.LogDebug("Using cached credentials for {Username} on server {ServerId}", identityUsername, serverId);
                return CreateGitHubClient(serverId, cachedCredentials);
            }
            
            _logger.LogDebug("Using default GitHub client for server {ServerId}", serverId);
            return CreateGitHubClient(serverId);
        }

        public async Task<Repository> CreateRepositoryAsync(
            string repoName,
            string organizationName,
            string description,
            bool isPrivate,
            int serverId)
        {
            Repository repository = null;
            try
            {
                var client = GetGitHubClient(serverId);
                
                var newRepo = new NewRepository(repoName)
                {
                    Description = description,
                    Private = isPrivate,
                    AutoInit = true
                };

                if (!string.IsNullOrEmpty(organizationName))
                {
                    repository = await client.Repository.Create(organizationName, newRepo);
                    _logger.LogInformation("Repository {RepoName} created in organization {OrgName}: {RepoUrl}", 
                        repoName, organizationName, repository.HtmlUrl);
                }
                else
                {
                    repository = await client.Repository.Create(newRepo);
                    _logger.LogInformation("Repository {RepoName} created: {RepoUrl}", 
                        repoName, repository.HtmlUrl);
                }
                
                // Create Azure DevOps pipeline without requiring a YAML file
                if (repository != null)
                {
                    string pipelineFilePath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Templates", "azure-pipelines.yml");
                    if (File.Exists(pipelineFilePath))
                    {
                        string yamlContent = await File.ReadAllTextAsync(pipelineFilePath);

                        // Push it to the default branch as "azure-pipelines.yml"
                        bool fileAdded = await AddFileToRepositoryAsync(
                            repository.Owner.Login,
                            repository.Name,
                            "azure-pipelines.yml",
                            yamlContent,
                            "Add Azure Pipelines configuration",
                            serverId);

                        if (fileAdded)
                        {
                            _logger.LogInformation("Added azure-pipelines.yml to repository {RepoName}", repoName);
                        }
                        else
                        {
                            _logger.LogWarning("Failed to add azure-pipelines.yml to repository {RepoName}", repoName);
                        }
                    }
                    else
                    {
                        _logger.LogWarning("Azure pipeline template file not found at {FilePath}", pipelineFilePath);
                    }

                    string azureOrgName = _configuration["AzureDevOps:Organization"];
                    string azureProjectName = _configuration["AzureDevOps:Project"];
                    
                    if (!string.IsNullOrEmpty(azureOrgName) && !string.IsNullOrEmpty(azureProjectName))
                    {
                        string pipelineUrl = await _azureDevOpsService.CreateAndRunPipelineAsync(
                            azureOrgName,
                            azureProjectName,
                            repository.HtmlUrl,
                            repository.Name);
                            
                        if (!string.IsNullOrEmpty(pipelineUrl))
                        {
                            _logger.LogInformation("Created and started Azure Pipeline for repository {RepoName}: {PipelineUrl}", 
                                repoName, pipelineUrl);
                        }
                        else
                        {
                            _logger.LogWarning("Failed to create Azure Pipeline for repository {RepoName}", repoName);
                        }
                    }
                    else
                    {
                        _logger.LogWarning("Azure DevOps organization or project not configured. Cannot create pipeline.");
                    }
                }
                
                return repository;
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Failed to create repository '{RepoName}'", repoName);
                if (repository != null)
                {
                    _logger.LogInformation("Repository '{RepoName}' had URL: {RepoUrl}", 
                        repoName, repository.HtmlUrl);
                }
                throw;
            }
        }
        public async Task<bool> AddFileToRepositoryAsync(
            string repositoryOwner,
            string repositoryName,
            string filePath,
            string fileContent,
            string commitMessage,
            int serverId)
        {
            try
            {
                var client = GetGitHubClient(serverId);

                _logger.LogDebug("Adding file with content:\n{Content}", fileContent);
                
                byte[] contentBytes = Encoding.UTF8.GetBytes(fileContent);
                
                var masterRef = await client.Git.Reference.Get(repositoryOwner, repositoryName, "heads/master");
                
                var latestCommit = await client.Git.Commit.Get(repositoryOwner, repositoryName, masterRef.Object.Sha);
                
                var newBlob = new NewBlob { 
                    Content = fileContent,
                    Encoding = EncodingType.Utf8
                };
                
                var blob = await client.Git.Blob.Create(repositoryOwner, repositoryName, newBlob);
                
                var newTree = new NewTree { 
                    BaseTree = latestCommit.Tree.Sha 
                };
                
                newTree.Tree.Add(new NewTreeItem { 
                    Path = filePath,
                    Mode = "100644",
                    Type = TreeType.Blob,
                    Sha = blob.Sha
                });
                
                var tree = await client.Git.Tree.Create(repositoryOwner, repositoryName, newTree);
                
                var newCommit = new NewCommit(commitMessage, tree.Sha, masterRef.Object.Sha);
                var commit = await client.Git.Commit.Create(repositoryOwner, repositoryName, newCommit);
                
                var refUpdate = new ReferenceUpdate(commit.Sha);
                await client.Git.Reference.Update(repositoryOwner, repositoryName, "heads/master", refUpdate);
                
                _logger.LogInformation("File '{FilePath}' created in repository {Owner}/{Repo}", 
                    filePath, repositoryOwner, repositoryName);
                
                return true;
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Failed to create file '{FilePath}' in repository {Owner}/{Repo}. Error: {Message}", 
                    filePath, repositoryOwner, repositoryName, ex.Message);
                return false;
            }
        }

        public async Task<IReadOnlyList<Organization>> GetOrganizationsAsync(int serverId)
        {
            try
            {
                var client = GetGitHubClient(serverId);
                
                var options = new ApiOptions
                {
                    PageSize = 100,
                    PageCount = 1
                };
                
                var organizations = await client.Organization.GetAllForCurrent(options);
                _logger.LogInformation("Retrieved {Count} organizations from server {ServerId}", 
                    organizations.Count, serverId);
                
                return organizations;
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Failed to retrieve organizations from server {ServerId}", serverId);
                throw;
            }
        }
    }
}


------------------

info: Server.Services.AzureDevOpsService[0]
      Creating Azure Pipeline for repository https://githubext.cae.com/Sandbox/azurepipelineautomator15
info: Server.Services.AzureDevOpsService[0]
      Default branch for repository azurepipelineautomator15 is master
info: System.Net.Http.HttpClient.AzureDevOps.LogicalHandler[100]
      Start processing HTTP request POST https://dev.azure.com/caeglobal/Engineering/_apis/build/definitions?*
info: System.Net.Http.HttpClient.AzureDevOps.ClientHandler[100]
      Sending HTTP request POST https://dev.azure.com/caeglobal/Engineering/_apis/build/definitions?*
info: System.Net.Http.HttpClient.AzureDevOps.ClientHandler[101]
      Received HTTP response headers after 1373.4506ms - 200
info: System.Net.Http.HttpClient.AzureDevOps.LogicalHandler[101]
      End processing HTTP request after 1373.6432ms - 200
info: System.Net.Http.HttpClient.AzureDevOps.LogicalHandler[100]
      Start processing HTTP request POST https://dev.azure.com/caeglobal/Engineering/_apis/build/builds?*
info: System.Net.Http.HttpClient.AzureDevOps.ClientHandler[100]
      Sending HTTP request POST https://dev.azure.com/caeglobal/Engineering/_apis/build/builds?*
info: System.Net.Http.HttpClient.AzureDevOps.ClientHandler[101]
      Received HTTP response headers after 396.8716ms - 400
info: System.Net.Http.HttpClient.AzureDevOps.LogicalHandler[101]
      End processing HTTP request after 396.9808ms - 400
fail: Server.Services.AzureDevOpsService[0]
      Failed to run build. Status: BadRequest. Response: {"$id":"1","customProperties":{"ValidationResults":[{"result":"error","message":"An error occurred while loading the YAML build pipeline. Could not find /azure-pipelines.yml in repository self hosted on https://githubext.cae.com/ using commit c58d9afaa77fb48f38eb765eab4e7165270e62c3. GitHub reported the error, \"Not Found\""}]},"innerException":null,"message":"Could 
not queue the build because there were validation errors or warnings.","typeName":"Microsoft.TeamFoundation.Build.WebApi.BuildRequestValidationFailedException, Microsoft.TeamFoundation.Build2.WebApi","typeKey":"BuildRequestValidationFailedException","errorCode":0,"eventId":3000}
warn: Server.Services.AzureDevOpsService[0]
      Repository might still be initializing. YAML file or branch not found.
warn: Server.Services.GitHubEnterpriseService[0]
      Failed to create Azure Pipeline for repository azurepipelineautomator15
GitHub repository created: https://githubext.cae.com/Sandbox/azurepipelineautomator15



ms.vss-web.platform-content.es6.tcm9Ik.min.js:1  ContributionData unavailable for 'ms.vss-build-web.check-for-hosted-mac-data-provider', ExceptionType: 'YamlFileNotFoundException', Reason: Could not find /azure-pipelines.yml in repository self hosted on https://githubext.cae.com/ using commit . GitHub reported the error, "Not Found", StackTrace: null
